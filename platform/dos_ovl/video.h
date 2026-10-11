/* VESA/VGA output for GLIDE2X.OVL (docs/technical-reference.md, section
 * 6). Deliberately split in two:
 *
 *  - video.c: all logic (parse VBE structures, choose the mode, convert
 *    and scale RGB565/XRGB8888 into the target format, mode 13h fallback
 *    with RGB332 palette, restore text mode). Portable, not a single BIOS
 *    or DPMI call; also runs on Linux (tests/video_test.c).
 *  - video_dpmi.c: the thin hardware layer (vid_bios), Watcom/DOS only:
 *    simulated real-mode INT 10h calls via DPMI, physical mapping of the
 *    LFB, port I/O for the DAC palette.
 *
 * vid_bios is replaceable so tools/leload_demo can run the real OVL against
 * a simulated BIOS (a Linux process has no BIOS interrupts). All function
 * pointers are explicitly cdecl: Watcom's default (-3s, "watcall stack")
 * preserves different registers than GCC's i386 cdecl. */

#ifndef GLIDULATOR_VIDEO_H
#define GLIDULATOR_VIDEO_H

#include <stddef.h>
#include <stdint.h>

#if defined(__WATCOMC__)
#    define VID_CALL __cdecl
#elif defined(__GNUC__) && defined(__i386__)
#    define VID_CALL __attribute__((cdecl))
#else
#    define VID_CALL
#endif

/* Register block for a simulated real-mode interrupt: exactly the layout
 * of DPMI INT 31h/AX=0300h (50 bytes), so video_dpmi.c passes it through
 * unchanged. */
typedef struct {
    uint32_t edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    uint16_t flags, es, ds, fs, gs, ip, cs, sp, ss;
} vid_rmregs;

typedef struct vid_bios {
    /* Real-mode INT 10h with the registers in *r (result back in *r). */
    void (VID_CALL *int10)(vid_rmregs *r);
    /* Real-mode address seg:off -> usable pointer (flat under DOS/4G:
     * seg*16+off). */
    void *(VID_CALL *real_ptr)(uint16_t seg, uint16_t off);
    /* Map a physical address (LFB) into the own address space, NULL on
     * failure. */
    void *(VID_CALL *map_phys)(uint32_t phys, uint32_t size);
    void (VID_CALL *outb)(uint16_t port, uint8_t value);
    /* buffer below 1 MB for VBE structures, at least VID_DOSBUF_BYTES. */
    uint16_t dosbuf_seg;
    /* Write-combining for the LFB (wc_dos.c, real OVL only; NULL = none):
     * on(phys, bytes used, video memory per VBE) -> 1 on, 0 already on,
     * < 0 impossible; off() restores the original state. */
    int (VID_CALL *wc_on)(uint32_t phys, uint32_t need, uint32_t vram);
    void (VID_CALL *wc_off)(void);
} vid_bios;

#define VID_DOSBUF_BYTES 1024 /* 512 ControllerInfo + 256 ModeInfo + slack */

/* One target format. bpp 8 always means mode 13h with the 6x7x6 palette
 * (vid_vga_palette). */
typedef struct {
    int      width, height, pitch, bpp;
    uint8_t  r_size, r_pos, g_size, g_pos, b_size, b_pos;
    uint32_t phys_base;
    uint16_t mode; /* VBE mode number (without LFB bit) or 0x13 */
} vid_mode;

/* VBE ModeInfoBlock (256 bytes) -> vid_mode. Returns 0 if the mode is
 * unusable (unsupported, not graphics, no LFB, not direct colour with
 * 15/16/24/32 bits). vbe3: use LinBytesPerScanLine instead of
 * BytesPerScanLine. */
int vid_parse_mode_info(const uint8_t *mib, int vbe3, vid_mode *out);

/* Index of the best mode for w x h output, -1 if none fits. Prefers the
 * smallest area >= w x h, at equal size the cheapest pixel format (RGB565 =
 * plain copy; with vid_prefer_deep 32/24 bit). */
int vid_select_mode(const vid_mode *modes, int n, int w, int h);

/* Target rectangle in the mode for a w x h source: larger modes scale by
 * an integer factor, centred; smaller ones (mode 13h only) are stretched to
 * the whole screen (see video.c). */
void vid_dest_rect(const vid_mode *m, int w, int h, int *x, int *y, int *rw, int *rh);

/* The same according to GLIDULATOR_SCALE (fit = OUT_SCALE_*); a source
 * larger than the mode (mode 13h) is always stretched to the whole
 * screen. */
void vid_fit_rect(const vid_mode *m, int fit, int w, int h, int *x, int *y, int *rw, int *rh);

/* Index of the mode with exactly out_w x out_h (and out_bpp if != 0;
 * otherwise the cheapest format), -1 if there is none. */
int vid_select_exact(const vid_mode *modes, int n, int out_w, int out_h, int out_bpp);

/* Copy an RGB565 source (w x h, src_stride bytes) into rectangle
 * (x,y,rw,rh) of the target, nearest-neighbour scaled and converted to the
 * format of m. Mode 13h instead averages the area each target pixel
 * covers (this cancels the Voodoo's 4x4 dither) and dithers once more to
 * the palette (vid_options.vga_dither). */
void vid_blit(const vid_mode *m, uint8_t *dst, int x, int y, int rw, int rh,
              const uint16_t *src, int w, int h, int src_stride);

/* Mode 13h palette: 6 red x 7 green x 6 blue levels, denser in the dark,
 * index = r * 42 + g * 6 + b (0..251, the rest black); 768 bytes of 6-bit
 * DAC values. */
#define VID_VGA_R 6
#define VID_VGA_G 7
#define VID_VGA_B 6
void vid_vga_palette(uint8_t pal[768]);

/* Gamma table for every output path (vid_blit, vid_blit32, mode 13h);
 * NULL = linear. Takes effect with the next vid_present(). */
void vid_set_gamma(const uint8_t lut[256]);

/* --- Output stages ---------------------------------------------------
 * vid_present() runs through fixed stages:
 *
 *   [1] scaler   into the target rectangle incl. format conversion
 *                (vid_scale_fn)
 *   [2] overlay  directly in the target format (vid_overlay_fn)
 *
 * Only VID_SCALE_NEAREST (= vid_blit) and no overlay are implemented;
 * vid_set_options() rejects everything else. A new filter = one function
 * with the matching signature + one entry in the table in video.c.
 * Anti-aliasing and the video filter happen before output, in core/. */

typedef enum {
    VID_SCALE_NEAREST = 0, /* implemented */
    VID_SCALE_BILINEAR,    /* not implemented */
    VID_SCALE_SCALE2X,     /* not implemented (EPX/Scale2x) */
    VID_SCALE_COUNT
} vid_scaler_id;

typedef enum {
    VID_OVERLAY_NONE = 0,  /* implemented */
    VID_OVERLAY_SCANLINES, /* not implemented */
    VID_OVERLAY_COUNT
} vid_overlay_id;

typedef enum {
    VID_DITHER_BLUE = 0, /* 16x16 blue-noise threshold matrix (default) */
    VID_DITHER_BAYER,    /* 4x4 Bayer matrix */
    VID_DITHER_OFF,      /* round to the nearest palette colour */
    VID_DITHER_COUNT
} vid_dither_id;

typedef struct {
    int ssaa;             /* must be 1 (anti-aliasing is done in core/fsaa.c) */
    int scaler;           /* vid_scaler_id */
    int out_w, out_h;     /* requested screen mode (GLIDULATOR_DISPLAY), 0 = as Glide */
    int out_bpp;          /* requested colour depth, 0 = best available */
    int fit;              /* picture in the mode: OUT_SCALE_* (GLIDULATOR_SCALE,
                           * platform/common/output_rect.h), default aspect */
    int wc;               /* try write-combining for the LFB (GLIDULATOR_WC), default 1 */
    int overlay;          /* vid_overlay_id */
    int overlay_strength; /* 0..255, meaning depends on the overlay */
    int vga_only;         /* mode 13h only, no VBE LFB (GLIDULATOR_DISPLAY=vga) */
    int vga_dither;       /* vid_dither_id for mode 13h (GLIDULATOR_VGA_DITHER) */
} vid_options;

/* Signature of every scaler stage, identical to vid_blit(). */
typedef void (*vid_scale_fn)(const vid_mode *m, uint8_t *dst, int x, int y, int rw, int rh,
                             const uint16_t *src, int w, int h, int src_stride);
/* The overlay works directly on the target rectangle after scaling. */
typedef void (*vid_overlay_fn)(const vid_mode *m, uint8_t *dst, int x, int y, int rw, int rh,
                               int strength);

void vid_default_options(vid_options *o);
/* 1 = accepted (takes effect at the next vid_open()); 0 = unsupported,
 * previous options stay. */
int vid_set_options(const vid_options *o);
/* Current options (e.g. to change a single one). */
void vid_get_options(vid_options *o);

/* --- State machine used by targets/glide_ovl ------------------------ */

/* NULL = headless (show nothing). Only change while the display is closed. */
void vid_set_bios(const vid_bios *bios);
/* Choose and set the mode for w x h (VBE LFB, else mode 13h, else
 * nothing). Remembers the previous BIOS mode. */
void vid_open(int w, int h);
void vid_present(const void *pixels, int w, int h, int stride_bytes);
/* Source XRGB8888 (video filter); vid_prefer_deep(1) before vid_open
 * prefers 32/24-bit modes over RGB565. */
void vid_present32(const uint32_t *pixels, int w, int h, int stride_bytes);
void vid_prefer_deep(int on);
/* 1 if the program has switched the screen mode itself since vid_open. */
int vid_mode_lost(void);
/* Restore the previous mode (usually text mode 3), unless the program has
 * already switched away. */
void vid_close(void);
/* Current mode, NULL if no output is active (for tests). */
const vid_mode *vid_current(void);

/* The real DPMI hardware layer (video_dpmi.c, Watcom only). NULL if no DOS
 * memory for the VBE structures could be obtained. */
const vid_bios *vid_dpmi_bios(void);

#endif /* GLIDULATOR_VIDEO_H */
