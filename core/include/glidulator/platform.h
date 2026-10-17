#ifndef GLIDULATOR_PLATFORM_H
#define GLIDULATOR_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The only place where core/ learns about the operating system. Every
 * platform (linux, win98, dos, dos_ovl) implements exactly these functions.
 * See docs/technical-reference.md.
 */

/* Cores (hardware threads) available for render workers, including the
 * calling one (1 = no others). */
int platform_core_count(void);

void *platform_alloc(size_t n);
void platform_free(void *p);

/* Readable, writable and executable memory for generated code (JIT,
 * core/rasterizer/vid_voodoo_codegen_x86.h). NULL if unsupported. */
void *platform_alloc_exec(size_t n);
void platform_free_exec(void *p, size_t n);

uint64_t platform_time_us(void);

/* Instruction set extensions core/ MAY use: the CPU has them (CPUID) AND
 * the system preserves their registers across task switches (SSE/SSE2 only
 * with FXSAVE support, i.e. CR4.OSFXSR). Drives the choice of the render
 * path (core/renderer.c). */
#define PLATFORM_CPU_MMX  0x1u
#define PLATFORM_CPU_SSE  0x2u
#define PLATFORM_CPU_SSE2 0x4u
unsigned platform_cpu_features(void);

typedef void (*platform_worker_fn)(void *arg);
typedef struct platform_worker platform_worker;

/* Run fn(arg) on another core/thread (core_id 1, 2, ...: worker number).
 * Returns NULL if the platform cannot; core/ then renders on one core
 * (core/render_mt.c). fn runs until it returns; platform_join_worker()
 * waits for that and releases the worker. Linux/Windows: threads; DOS OVL:
 * additional CPU cores started by the OVL itself (ring 0). */
platform_worker *platform_spawn_worker(int core_id, platform_worker_fn fn, void *arg);
void platform_join_worker(platform_worker *w);

/* Safety net (core/render_mt.c). platform_worker_failed() returns 1 if the
 * worker died (DOS OVL: CPU exception on its core) and will never return.
 * platform_kill_worker() abandons a hung worker (DOS OVL: its core is reset
 * by INIT when the cores are stopped); platform_join_worker() then returns
 * immediately. Both return 0 where this is impossible (threads on
 * Linux/Windows), and the caller keeps waiting. */
int platform_worker_failed(platform_worker *w);
int platform_kill_worker(platform_worker *w);

/* Vsync (grBufferSwap with swap_interval n > 0): wait for the start of the
 * n-th next vertical retrace. Returns 1 if waited, 0 if unsupported (the
 * frame is shown without waiting). DOS OVL: VGA port 3DAh; Linux/Windows:
 * not implemented. */
int platform_wait_vretrace(int n);

/* Show a frame. pixels: packed RGB565, the format the rasterizer writes. */
void platform_blit(const void *pixels, int w, int h, int stride_bytes);

/* The same for XRGB8888 (0x00RRGGBB, before gamma). Used only by the Voodoo
 * video filter (core/vfilter.c), whose result has more than 16 bits. */
void platform_blit32(const uint32_t *pixels, int w, int h, int stride_bytes);

/* grSstSelect(): last Glide call before wrappers such as the 3dfx MiniGL
 * size the Glide screen from the program window's client rectangle.
 * Windows: restores the calling thread's active window if it is minimized
 * (0 x 0 client area). Other platforms: nothing to do. */
void platform_prepare_window(void);

/* Called before platform_display_open(): 1 = prefer 24/32-bit screen modes
 * (video filter on), 0 = prefer RGB565 modes. */
void platform_display_prefer_deep(int on);

/* Set up the display for w x h output (grSstWinOpen()), or tear it down and
 * restore the previous screen mode (grSstWinClose()). May fail without
 * failing Glide calls; platform_blit() then shows nothing. */
void platform_display_open(int w, int h);
void platform_display_close(void);

/* 1 if the program has left the Glide screen mode on its own (DOS: back to
 * text mode before grSstWinClose); then nothing more may be drawn. */
int platform_display_lost(void);

/* Screenshot key (core/screenshot.c): 1 if it was pressed since the last
 * call. DOS OVL: Print Screen, through a hook in front of the game's
 * keyboard handler while the display is open (GLIDULATOR_SCREENSHOT=0
 * turns it off); Linux: set by the test program; otherwise 0. */
int platform_screenshot_key(void);

/* Actual screen mode size; returns 1 if known. Used by the OSD only. */
int platform_display_size(int *w, int *h);

/* Output gamma table, like the Voodoo RAMDAC: every 8-bit channel is
 * replaced by lut[] on output; the framebuffer itself stays linear.
 * Computed by grGammaCorrectionValue (core/glide_sst.c). */
void platform_set_gamma(const uint8_t lut[256]);

/* User setting, like the environment variables of the 3dfx driver (e.g.
 * "SST_GAMMA"): value into buf (zero-terminated), returns 1 if set.
 * Linux/DJGPP: environment; Windows: environment, then GLIDULAT.CFG in the
 * current directory; DOS OVL (no C runtime): GLIDULAT.CFG only (format in
 * glidulator/config.h). */
int platform_get_config(const char *name, char *buf, size_t n);

/* Write one diagnostic line (including '\n') to wherever the platform logs
 * (stderr, debug output, GLIDULAT.LOG). Trace/debug variants only. */
void platform_log(const char *line);

/* Create/overwrite a file (frame capture of the debug variant,
 * core/capture.c). Returns 0 on success. */
int platform_write_file(const char *name, const void *data, size_t len);

/* Read up to len bytes from byte offset of file name into buf (gu3dfLoad,
 * config files). Returns the number of bytes read (fewer at end of file),
 * -1 if the file cannot be opened. */
long platform_read_file(const char *name, size_t offset, void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* GLIDULATOR_PLATFORM_H */
