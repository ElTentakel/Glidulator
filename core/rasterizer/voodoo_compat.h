#ifndef GLIDULATOR_VOODOO_COMPAT_H
#define GLIDULATOR_VOODOO_COMPAT_H

/*
 * Replaces the 86Box environment (86box.h, cpu.h, thread.h, plat.h, ...)
 * that the original vid_voodoo_render.c/vid_voodoo_texture.c expect. See
 * ATTRIBUTION.md for the complete list of changes.
 *
 * There is deliberately no thread/mutex/event backend: 86Box's render
 * thread machinery was removed, and Glidulator's own workers
 * (core/render_mt.c on platform_spawn_worker()) replace it.
 * thread_t/mutex_t/event_t/pc_timer_t remain as placeholder types only, so
 * vid_voodoo_common.h stays unchanged.
 */

#include "glidulator/platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MIN
#    define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#    define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* NO_CODEGEN is decided by vid_voodoo_render.h (JIT on 32-bit x86 only). */

/* core/render_mt.c: render workers (the texture cache waits for them) */
int  glide_mt_workers(void);
void glide_mt_idle(void);
#define ATOMIC_INT  volatile int /* volatile suffices: aligned int accesses are atomic on x86, and the worker queue relies on TSO ordering (core/render_mt.c) */
#define ATOMIC_UINT volatile unsigned int
#ifdef __GNUC__
#    define UNUSED(arg) __attribute__((unused)) arg
#else
#    define UNUSED(arg) arg /* Open Watcom does not know __attribute__ */
#endif

typedef struct thread_t thread_t;
typedef struct mutex_t mutex_t;
typedef struct event_t event_t;
typedef uint64_t pc_timer_t;
typedef struct svga_t svga_t; /* only referenced as a pointer inside the struct, never dereferenced */

/* mem_mapping_t is embedded BY VALUE in voodoo_t (86Box's PCI MMIO
 * registration, unused here because no register MMIO is emulated), so it
 * needs a complete, if empty, definition. */
typedef struct mem_mapping_t {
    int _glidulator_unused;
} mem_mapping_t;

static inline void
glr_log(const char *fmt, ...) {
#ifdef GLIDULATOR_RASTERIZER_VERBOSE
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
#else
    (void)fmt;
#endif
}

/* vid_voodoo_tables.c: call once before the first use of
 * voodoo_triangle()/voodoo_use_texture() (see ATTRIBUTION.md). */
void voodoo_init_color_tables(void);

static inline void
glr_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}

#ifdef __cplusplus
}
#endif

#endif /* GLIDULATOR_VOODOO_COMPAT_H */
