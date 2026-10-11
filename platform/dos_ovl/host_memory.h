/* Memory hook of the DOS OVL (platform/dos_ovl/platform_watcom.c):
 * normally platform_alloc() gets memory via DPMI; tools/leload_demo (Linux,
 * no INT 31h) installs the host's malloc/free through the export
 * glidulatorDebugSetMemory() instead. cdecl on purpose: the caller is built
 * with GCC (like vid_bios in video.h). */
#ifndef GLIDULATOR_DOS_OVL_HOST_MEMORY_H
#define GLIDULATOR_DOS_OVL_HOST_MEMORY_H

#include <stddef.h>

#if defined(__WATCOMC__)
#    define HOST_CALL __cdecl
#elif defined(__GNUC__) && defined(__i386__)
#    define HOST_CALL __attribute__((cdecl))
#else
#    define HOST_CALL
#endif

typedef void *(HOST_CALL *platform_host_alloc_fn)(size_t n);
typedef void (HOST_CALL *platform_host_free_fn)(void *p);

/* NULL, NULL = DPMI again. */
void platform_set_host_memory(platform_host_alloc_fn alloc, platform_host_free_fn free_fn);

/* 1 if the hook is set (then no DOS, no ring 0: smp_dos.c) */
int platform_host_memory_set(void);

#endif
