/* CPUID evaluation for the GCC platforms (Linux, mingw, DJGPP), see
 * platform_cpu_features() in glidulator/platform.h. Reports what the CPU
 * can do; whether the OS saves SSE registers is decided by each platform
 * (os_sse). */

#ifndef GLIDULATOR_CPU_GCC_H
#define GLIDULATOR_CPU_GCC_H

#include "glidulator/platform.h"

#if defined(__i386__) || defined(__x86_64__)
#    include <cpuid.h>
#endif

static inline unsigned cpu_features_gcc(int os_sse) {
#if defined(__i386__) || defined(__x86_64__)
    unsigned a, b, c, d, f = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d)) /* also checks the EFLAGS ID bit (386/486) */
        return 0;
    if (d & (1u << 23))
        f |= PLATFORM_CPU_MMX;
    if (os_sse && (d & (1u << 25)))
        f |= PLATFORM_CPU_SSE;
    if (os_sse && (d & (1u << 26)))
        f |= PLATFORM_CPU_SSE2;
    return f;
#else
    (void) os_sse;
    return 0;
#endif
}

#endif /* GLIDULATOR_CPU_GCC_H */
