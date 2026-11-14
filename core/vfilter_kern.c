/* Kernels of the Voodoo video filter, see vfilter_kern.h */

#include "vfilter_kern.h"

void vf_make_table(uint8_t t[256][256], int cap) {
    for (int g = 0; g < 256; g++)
        for (int h = 0; h < 256; h++) {
            int d = h - g;
            if (d > cap)
                d = cap;
            if (d < -cap)
                d = -cap;
            int v = g + d / 2;
            t[g][h] = (uint8_t) (v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

void vf_split565(const uint16_t *src, uint8_t *r, uint8_t *g, uint8_t *b, int n) {
    for (int x = 0; x < n; x++) {
        unsigned p = src[x];
        r[x]       = (uint8_t) (((p >> 11) & 31) << 3);
        g[x]       = (uint8_t) (((p >> 5) & 63) << 2);
        b[x]       = (uint8_t) ((p & 31) << 3);
    }
}

void vf_join(const uint8_t *r, const uint8_t *g, const uint8_t *b, uint32_t *dst, int n) {
    for (int x = 0; x < n; x++)
        dst[x] = (uint32_t) r[x] << 16 | (uint32_t) g[x] << 8 | b[x];
}

void vf_pass_table(uint8_t *dst, const uint8_t *g, const uint8_t *h, int n, uint8_t (*t)[256]) {
    for (int i = 0; i < n; i++)
        dst[i] = t[g[i]][h[i]];
}

#if VF_HAVE_SSE2
#include <emmintrin.h>

/* Per 8 pixels in 16-bit lanes: d = h - g, clamp to +-cap, d / 2 rounded
 * towards zero ((d + (d < 0)) >> 1, arithmetic), plus g. The result lies
 * between g and h, so packus to bytes never saturates. */
__attribute__((target("sse2"))) static inline __m128i vf_step8(__m128i g, __m128i h, __m128i cap, __m128i ncap) {
    __m128i d = _mm_sub_epi16(h, g);
    d         = _mm_min_epi16(_mm_max_epi16(d, ncap), cap);
    d         = _mm_srai_epi16(_mm_add_epi16(d, _mm_srli_epi16(d, 15)), 1);
    return _mm_add_epi16(g, d);
}

__attribute__((target("sse2"))) void vf_pass_sse2(uint8_t *dst, const uint8_t *g, const uint8_t *h, int n, int cap) {
    const __m128i z  = _mm_setzero_si128();
    const __m128i c  = _mm_set1_epi16((short) cap);
    const __m128i nc = _mm_set1_epi16((short) -cap);
    int           i  = 0;
    for (; i + 16 <= n; i += 16) {
        __m128i gv = _mm_loadu_si128((const __m128i *) (g + i));
        __m128i hv = _mm_loadu_si128((const __m128i *) (h + i));
        __m128i lo = vf_step8(_mm_unpacklo_epi8(gv, z), _mm_unpacklo_epi8(hv, z), c, nc);
        __m128i hi = vf_step8(_mm_unpackhi_epi8(gv, z), _mm_unpackhi_epi8(hv, z), c, nc);
        _mm_storeu_si128((__m128i *) (dst + i), _mm_packus_epi16(lo, hi));
    }
    for (; i < n; i++) {
        int d = h[i] - g[i];
        if (d > cap)
            d = cap;
        if (d < -cap)
            d = -cap;
        dst[i] = (uint8_t) (g[i] + d / 2);
    }
}

/* 16 pixels: r = (p >> 8) & F8, g = (p >> 3) & FC, b = (p << 3) & F8 in
 * 16-bit lanes, packed to bytes (values <= 255, no saturation) */
__attribute__((target("sse2"))) void vf_split565_sse2(const uint16_t *src, uint8_t *r, uint8_t *g, uint8_t *b, int n) {
    const __m128i mr = _mm_set1_epi16(0xf8), mg = _mm_set1_epi16(0xfc);
    int           x  = 0;
    for (; x + 16 <= n; x += 16) {
        __m128i p0 = _mm_loadu_si128((const __m128i *) (src + x));
        __m128i p1 = _mm_loadu_si128((const __m128i *) (src + x + 8));
        _mm_storeu_si128((__m128i *) (r + x), _mm_packus_epi16(_mm_and_si128(_mm_srli_epi16(p0, 8), mr),
                                                                _mm_and_si128(_mm_srli_epi16(p1, 8), mr)));
        _mm_storeu_si128((__m128i *) (g + x), _mm_packus_epi16(_mm_and_si128(_mm_srli_epi16(p0, 3), mg),
                                                                _mm_and_si128(_mm_srli_epi16(p1, 3), mg)));
        _mm_storeu_si128((__m128i *) (b + x), _mm_packus_epi16(_mm_and_si128(_mm_slli_epi16(p0, 3), mr),
                                                                _mm_and_si128(_mm_slli_epi16(p1, 3), mr)));
    }
    vf_split565(src + x, r + x, g + x, b + x, n - x);
}

/* 16 pixels: bytes b, g, r, 0 per pixel (little endian XRGB8888) */
__attribute__((target("sse2"))) void vf_join_sse2(const uint8_t *r, const uint8_t *g, const uint8_t *b, uint32_t *dst, int n) {
    const __m128i z = _mm_setzero_si128();
    int           x = 0;
    for (; x + 16 <= n; x += 16) {
        __m128i rv = _mm_loadu_si128((const __m128i *) (r + x));
        __m128i gv = _mm_loadu_si128((const __m128i *) (g + x));
        __m128i bv = _mm_loadu_si128((const __m128i *) (b + x));
        __m128i bg_lo = _mm_unpacklo_epi8(bv, gv), bg_hi = _mm_unpackhi_epi8(bv, gv);
        __m128i r0_lo = _mm_unpacklo_epi8(rv, z), r0_hi = _mm_unpackhi_epi8(rv, z);
        _mm_storeu_si128((__m128i *) (dst + x), _mm_unpacklo_epi16(bg_lo, r0_lo));
        _mm_storeu_si128((__m128i *) (dst + x + 4), _mm_unpackhi_epi16(bg_lo, r0_lo));
        _mm_storeu_si128((__m128i *) (dst + x + 8), _mm_unpacklo_epi16(bg_hi, r0_hi));
        _mm_storeu_si128((__m128i *) (dst + x + 12), _mm_unpackhi_epi16(bg_hi, r0_hi));
    }
    vf_join(r + x, g + x, b + x, dst + x, n - x);
}
#endif
