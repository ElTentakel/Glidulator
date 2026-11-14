/* Voodoo video filter kernels (core/vfilter_kern.c): the SSE2 pass must give
 * exactly the table pass of 86Box's filter, for every threshold, every
 * pixel/neighbour pair and every length (tail without SIMD). */

#include <stdio.h>
#include <string.h>

#include "../core/vfilter_kern.h"

static uint8_t tab[256][256];
static uint8_t g[65536 + 64], h[65536 + 64], ref[65536 + 64], out[65536 + 64];

int main(void) {
#if VF_HAVE_SSE2
    int fails = 0;
    for (int i = 0; i < 65536; i++) {
        g[i] = (uint8_t) (i >> 8);
        h[i] = (uint8_t) i;
    }
    for (int cap = 0; cap < 256; cap++) {
        vf_make_table(tab, cap);
        /* all pairs, and every length 0..40 from an odd start (tails,
         * unaligned loads) */
        vf_pass_table(ref, g, h, 65536, tab);
        memset(out, 0xa5, sizeof out);
        vf_pass_sse2(out, g, h, 65536, cap);
        if (memcmp(ref, out, 65536) || out[65536] != 0xa5) {
            printf("vfilter_test: cap %d: all pairs differ\n", cap);
            fails++;
        }
        for (int n = 0; n <= 40; n++) {
            memset(out, 0xa5, sizeof out);
            vf_pass_table(ref, g + 4097, h + 4097, n, tab);
            vf_pass_sse2(out, g + 4097, h + 4097, n, cap);
            if (memcmp(ref, out, (size_t) n) || out[n] != 0xa5) {
                printf("vfilter_test: cap %d, length %d differs\n", cap, n);
                fails++;
            }
        }
    }
    /* split/join: every RGB565 value, lengths 0-40 */
    static uint16_t px[65536 + 64];
    static uint8_t  r1[65600], g1[65600], b1[65600], r2[65600], g2[65600], b2[65600];
    static uint32_t j1[65600], j2[65600];
    for (int i = 0; i < 65536; i++)
        px[i] = (uint16_t) (i * 40503u);
    for (int n = 0; n <= 65536; n = n < 40 ? n + 1 : 65537) {
        memset(r2, 0xa5, sizeof r2);
        memset(j2, 0xa5, sizeof j2);
        vf_split565(px + 3, r1, g1, b1, n);
        vf_split565_sse2(px + 3, r2, g2, b2, n);
        vf_join(r1 + 1, g1, b1 + 2, j1, n > 2 ? n - 2 : 0);
        vf_join_sse2(r1 + 1, g1, b1 + 2, j2 + 1, n > 2 ? n - 2 : 0);
        if (memcmp(r1, r2, (size_t) n) || memcmp(g1, g2, (size_t) n) || memcmp(b1, b2, (size_t) n) || r2[n] != 0xa5 ||
            memcmp(j1, j2 + 1, (n > 2 ? (size_t) n - 2 : 0) * 4) || j2[0] != 0xa5a5a5a5u ||
            j2[1 + (n > 2 ? n - 2 : 0)] != 0xa5a5a5a5u) {
            printf("vfilter_test: split/join length %d differs\n", n);
            fails++;
        }
    }
    if (fails)
        return 1;
    printf("vfilter_test: SSE2 pass = table pass (256 thresholds, all pairs, lengths 0-40); split/join equal\n");
#else
    printf("vfilter_test: no SSE2 kernel in this build, skipped\n");
#endif
    return 0;
}
