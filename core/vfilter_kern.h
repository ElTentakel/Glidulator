/* Kernels of the Voodoo video filter (core/vfilter.c), separate so that
 * tests/vfilter_test.c can check them without the rest of the core.
 *
 * One filter step for a pixel g and its neighbour h (86Box's table,
 * voodoo_generate_filter_v1): difference h - g limited to +-cap, result
 * g + difference / 2 (C division, rounds towards zero). A pass applies it
 * to n pixels: dst[i] = step(g[i], h[i]). dst must not overlap g or h. */

#ifndef GLIDULATOR_VFILTER_KERN_H
#define GLIDULATOR_VFILTER_KERN_H

#include <stdint.h>

/* t[g][h] for one channel */
void vf_make_table(uint8_t t[256][256], int cap);

/* RGB565 line -> channels of 8 bits (r, g, b, value << 3 / << 2), and back
 * to XRGB8888. Scalar versions for every build. */
void vf_split565(const uint16_t *src, uint8_t *r, uint8_t *g, uint8_t *b, int n);
void vf_join(const uint8_t *r, const uint8_t *g, const uint8_t *b, uint32_t *dst, int n);

/* Reference pass through the table (any compiler and CPU) */
void vf_pass_table(uint8_t *dst, const uint8_t *g, const uint8_t *h, int n, uint8_t (*t)[256]);

/* Same result computed with SSE2, 16 pixels at a time. Only built with
 * GCC on x86 (VF_HAVE_SSE2 1); call it only when the CPU and the system
 * allow SSE2 (platform_cpu_features() & PLATFORM_CPU_SSE2). */
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#define VF_HAVE_SSE2 1
void vf_pass_sse2(uint8_t *dst, const uint8_t *g, const uint8_t *h, int n, int cap);
void vf_split565_sse2(const uint16_t *src, uint8_t *r, uint8_t *g, uint8_t *b, int n);
void vf_join_sse2(const uint8_t *r, const uint8_t *g, const uint8_t *b, uint32_t *dst, int n);
#else
#define VF_HAVE_SSE2 0
#endif

#endif
