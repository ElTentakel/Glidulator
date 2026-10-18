/* Plan write-combining for the framebuffer with MTRRs (portable, tested in
 * tests/mtrr_test.c; applied in ring 0 by platform/dos_ovl/wc_dos.c and
 * platform/win98/wc_win9x.c).
 *
 * Firmware typically sets the default type to WB and marks the whole PCI
 * window (e.g. 3-4 GB) UC with a variable MTRR. Where ranges overlap, UC
 * wins, so simply adding a WC range has no effect. Two layouts are
 * handled:
 *  - Default type WB (e.g. SeaBIOS): replace every UC range overlapping the
 *    framebuffer by UC pieces without it (aligned powers of two), add WC.
 *  - Default type UC with a large WB range and punched-out UC holes
 *    (typical Intel firmware): the UC hole H around the framebuffer needs
 *    no register of its own; the WB ranges are split around H, ranges
 *    inside H are dropped (the default type UC applies there), and only
 *    the framebuffer becomes WC.
 * Outside the framebuffer the memory type never changes (tested). If the
 * plan does not fit into the registers, or something unexpected overlaps,
 * nothing is changed. */

#ifndef GLIDULATOR_MTRR_PLAN_H
#define GLIDULATOR_MTRR_PLAN_H

#include <stdint.h>

#define MTRR_MAX_VAR 32
#define MTRR_UC      0
#define MTRR_WC      1
#define MTRR_WB      6

typedef struct {
    uint64_t base; /* IA32_MTRR_PHYSBASEn: address | type */
    uint64_t mask; /* IA32_MTRR_PHYSMASKn: mask | valid (bit 11) */
} mtrr_var;

typedef struct {
    int      count;     /* variable MTRRs per MTRRcap */
    int      phys_bits; /* physical address width (CPUID 80000008h, else 36) */
    int      def_type;  /* default type (MTRRdefType bits 0-7) */
    mtrr_var var[MTRR_MAX_VAR];
} mtrr_state;

/* result of mtrr_plan_wc() */
#define MTRR_PLAN_DONE     1  /* out holds the new layout */
#define MTRR_PLAN_ALREADY  0  /* range is WC already, nothing to do */
#define MTRR_PLAN_NOROOM  -1  /* not enough free registers */
#define MTRR_PLAN_CONFLICT -2 /* overlaps a range that is not UC */
#define MTRR_PLAN_BADARGS -3  /* range is not an aligned power of two */

/* WC for [base, base + size); size a power of two >= 4 KB, base aligned to
 * it. *used = registers in use afterwards. */
int mtrr_plan_wc(const mtrr_state *in, uint64_t base, uint64_t size, mtrr_state *out, int *used);

/* Largest sensible WC range for a framebuffer: at least need bytes, at
 * most vram (video memory per VBE), aligned at phys; the larger, the fewer
 * UC pieces. 0 if phys is not suitably aligned. */
uint64_t mtrr_wc_size(uint64_t phys, uint64_t need, uint64_t vram);

#endif /* GLIDULATOR_MTRR_PLAN_H */
