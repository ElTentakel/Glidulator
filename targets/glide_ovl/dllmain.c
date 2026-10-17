/*
 * GLIDE2X.OVL entry point (DOS/4G LE DLL), see README.md in this directory.
 *
 * The DOS extender calls __DLLstart_ exactly once at load time, before the
 * game calls any Glide function, with arguments that carry no reliable
 * values (documented by MGA-Glide: inst/reason are partly random with real
 * games). It is therefore a no-op: the actual initialisation happens in
 * grGlideInit(), which every game calls first anyway.
 */

#include "glidulator/glide2x.h"
#include "glidulator/platform.h"

#include "../../platform/dos_ovl/host_memory.h"
#include "../../platform/dos_ovl/video.h"
#include "../../platform/common/output_rect.h"

void ovl_diag_machine(const char *when); /* diag_dos.c, debug OVL only */
void keyb_dos_hook(void);                 /* keyb_dos.c: Print Screen */
void keyb_dos_unhook(void);

/* No calling convention or export marker: the extender looks for
 * "__DLLstart_" as an exact, undecorated symbol name ("option
 * start=__DLLstart_" in glide2x.lnk.in); __stdcall would decorate the name
 * and wlink would not find it. */
int __DLLstart_(void *inst, unsigned reason) {
    (void) inst;
    (void) reason;
    return 1;
}

/* Frame output: VBE LFB, else mode 13h, else none, see
 * platform/dos_ovl/video.h. */

static int g_bios_chosen;

static void choose_bios(void) {
    if (!g_bios_chosen) {
        vid_set_bios(vid_dpmi_bios());
        g_bios_chosen = 1;
    }
}

/* GLIDULATOR_DISPLAY (WxH[xBPP]; "glide"/"desktop" = as the game) and
 * GLIDULATOR_SCALE (aspect|stretch|integer|none) from GLIDULAT.CFG, as on
 * Windows. */
void platform_display_open(int w, int h) {
    char        cfg[24];
    vid_options o;
    choose_bios();
    vid_get_options(&o);
    o.out_w = o.out_h = o.out_bpp = 0;
    o.fit                         = OUT_SCALE_ASPECT;
    o.vga_only = 0;
    if (platform_get_config("GLIDULATOR_DISPLAY", cfg, sizeof cfg)) {
        /* "vga": mode 13h only through the legacy VGA window at A0000 (no
         * access to the graphics card's LFB) */
        if ((cfg[0] == 'v' || cfg[0] == 'V') && (cfg[1] == 'g' || cfg[1] == 'G') && (cfg[2] == 'a' || cfg[2] == 'A') && !cfg[3])
            o.vga_only = 1;
        else
            output_display_parse(cfg, &o.out_w, &o.out_h, &o.out_bpp);
    }
    if (!o.out_w || !o.out_h)
        o.out_w = o.out_h = 0;
    if (platform_get_config("GLIDULATOR_SCALE", cfg, sizeof cfg) && output_scale_parse(cfg) >= 0)
        o.fit = output_scale_parse(cfg);
    o.wc = !(platform_get_config("GLIDULATOR_WC", cfg, sizeof cfg) && cfg[0] == '0');
    /* GLIDULATOR_VGA_DITHER: blue (default) | bayer | off */
    o.vga_dither = VID_DITHER_BLUE;
    if (platform_get_config("GLIDULATOR_VGA_DITHER", cfg, sizeof cfg)) {
        char c = (char) (cfg[0] | 0x20);
        o.vga_dither = c == 'b' && (cfg[1] | 0x20) == 'a' ? VID_DITHER_BAYER
                       : c == 'o' || c == '0'              ? VID_DITHER_OFF
                                                           : VID_DITHER_BLUE;
    }
    vid_set_options(&o);
    ovl_diag_machine("Start");
    vid_open(w, h);
    keyb_dos_hook();
}

void platform_cpu_release(void); /* platform_watcom.c: restore CR4.OSFXSR */


void platform_display_close(void) {
    ovl_diag_machine("End");
    keyb_dos_unhook();
    platform_cpu_release();
    vid_close();
}

int platform_display_lost(void) {
    return vid_mode_lost();
}

int platform_display_size(int *w, int *h) {
    const vid_mode *m = vid_current();
    if (!m)
        return 0;
    *w = m->width;
    *h = m->height;
    return 1;
}

void platform_set_gamma(const uint8_t lut[256]) {
    vid_set_gamma(lut);
}

void platform_blit(const void *pixels, int w, int h, int stride_bytes) {
    vid_present(pixels, w, h, stride_bytes);
}

void platform_blit32(const uint32_t *pixels, int w, int h, int stride_bytes) {
    vid_present32(pixels, w, h, stride_bytes);
}

void platform_prepare_window(void) {
}

void platform_display_prefer_deep(int on) {
    vid_prefer_deep(on);
}

/* NOT Glide API: test hook for tools/leload_demo. Replaces the real DPMI
 * layer with a simulated BIOS (NULL = no output at all) before
 * grSstWinOpen() first needs it; a Linux process has no BIOS interrupts.
 * Returns the currently chosen mode (vid_current(), NULL if none) so the
 * demo can check it. */
const vid_mode *GR_EXPORT GLIDE_API glidulatorDebugSetVideoBios(const vid_bios *bios, int set) {
    if (set) {
        vid_set_bios(bios);
        g_bios_chosen = 1;
    }
    return vid_current();
}

/* NOT Glide API: test hook for tools/leload_demo. platform_alloc/
 * platform_free use the host's malloc/free instead of DPMI (Linux has no
 * INT 31h). Must be set before the first Glide call. */
void GR_EXPORT GLIDE_API glidulatorDebugSetMemory(platform_host_alloc_fn alloc, platform_host_free_fn free_fn) {
    platform_set_host_memory(alloc, free_fn);
}

/* Never called (the extender calls __DLLstart_, not main), but
 * clib3s.lib's startup module (cmain386.c) requires the symbol even for a
 * DLL target; without it the link fails with "main is an undefined
 * reference". */
int main(void) {
    return 0;
}
