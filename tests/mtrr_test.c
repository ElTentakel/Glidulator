/* Test of the MTRR planner (platform/dos_ovl/mtrr_plan.c). */
#include "../platform/dos_ovl/mtrr_plan.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint64_t g_am = 0xffffff000ull; /* address mask (36 bits, test machine 39) */

static mtrr_var var(uint64_t base, uint64_t size, int type) {
    mtrr_var v;
    v.base = base | (uint64_t) type;
    v.mask = (~(size - 1) & g_am) | 0x800;
    return v;
}

/* Type of address a by Intel's rules (UC wins; otherwise one match). */
static int type_at(const mtrr_state *s, uint64_t a, int def) {
    int hits = 0, t = def, uc = 0;
    for (int i = 0; i < s->count; i++) {
        const mtrr_var *v = &s->var[i];
        if (!(v->mask & 0x800))
            continue;
        uint64_t m = v->mask & g_am;
        if ((a & m) == (v->base & m)) {
            hits++;
            t = (int) (v->base & 0xff);
            uc |= t == MTRR_UC;
        }
    }
    return uc ? MTRR_UC : (hits > 1 ? -1 : t);
}

int main(void) {
    mtrr_state in, out;
    int        used;
    memset(&in, 0, sizeof in);
    in.count     = 8;
    in.phys_bits = 36;

    /* SeaBIOS/QEMU and many real boards: default WB, 3-4 GB UC */
    in.var[0] = var(0xC0000000ull, 0x40000000ull, MTRR_UC);
    CHECK(mtrr_plan_wc(&in, 0xE0000000ull, 0x1000000ull, &out, &used) == MTRR_PLAN_DONE);
    CHECK(used <= 8);
    CHECK(type_at(&out, 0xE0000000ull, MTRR_WB) == MTRR_WC);
    CHECK(type_at(&out, 0xE0FFF000ull, MTRR_WB) == MTRR_WC);
    CHECK(type_at(&out, 0xE1000000ull, MTRR_WB) == MTRR_UC);
    CHECK(type_at(&out, 0xDFFFF000ull, MTRR_WB) == MTRR_UC);
    CHECK(type_at(&out, 0xC0000000ull, MTRR_WB) == MTRR_UC);
    CHECK(type_at(&out, 0xFFFFF000ull, MTRR_WB) == MTRR_UC);
    CHECK(type_at(&out, 0x10000000ull, MTRR_WB) == MTRR_WB); /* RAM untouched */
    /* aligned to 256 MB: only 2 UC pieces + WC */
    CHECK(mtrr_plan_wc(&in, 0xE0000000ull, 0x10000000ull, &out, &used) == MTRR_PLAN_DONE && used == 3);

    /* typical Intel board: default UC, RAM as WB -> only add WC */
    memset(&in.var, 0, sizeof in.var);
    in.var[0] = var(0, 0x80000000ull, MTRR_WB);
    in.var[1] = var(0x80000000ull, 0x40000000ull, MTRR_WB);
    CHECK(mtrr_plan_wc(&in, 0xE0000000ull, 0x1000000ull, &out, &used) == MTRR_PLAN_DONE && used == 3);
    CHECK(type_at(&out, 0xE0000000ull, MTRR_UC) == MTRR_WC);
    CHECK(type_at(&out, 0x90000000ull, MTRR_UC) == MTRR_WB);

    /* already WC */
    in.var[2] = var(0xE0000000ull, 0x10000000ull, MTRR_WC);
    CHECK(mtrr_plan_wc(&in, 0xE0000000ull, 0x1000000ull, &out, &used) == MTRR_PLAN_ALREADY);

    /* WB over the framebuffer: do not touch */
    memset(&in.var, 0, sizeof in.var);
    in.var[0] = var(0xE0000000ull, 0x10000000ull, MTRR_WB);
    CHECK(mtrr_plan_wc(&in, 0xE0000000ull, 0x1000000ull, &out, &used) == MTRR_PLAN_CONFLICT);

    /* not enough registers */
    memset(&in.var, 0, sizeof in.var);
    in.count  = 3;
    in.var[0] = var(0xC0000000ull, 0x40000000ull, MTRR_UC);
    in.var[1] = var(0, 0x80000000ull, MTRR_WB);
    CHECK(mtrr_plan_wc(&in, 0xE0000000ull, 0x1000000ull, &out, &used) == MTRR_PLAN_NOROOM);
    CHECK(mtrr_plan_wc(&in, 0xE0001000ull, 0x2000ull, &out, &used) == MTRR_PLAN_BADARGS);

    /* size: at least need, at most vram, aligned */
    CHECK(mtrr_wc_size(0xE0000000ull, 960 * 1024, 64ull << 20) == 64ull << 20);
    CHECK(mtrr_wc_size(0xE0000000ull, 960 * 1024, 0) == 1ull << 20);
    CHECK(mtrr_wc_size(0xE0100000ull, 960 * 1024, 64ull << 20) == 1ull << 20);
    CHECK(mtrr_wc_size(0xE0080000ull, 960 * 1024, 64ull << 20) == 0);

    /* Real layout (Core i7-4770T): default UC, WB 0-8 GB, WB 8-8.5 GB, UC
     * holes 3.5-4 GB, DC000000/64 MB, DB000000/16 MB, 21FE00000/2 MB; 10
     * registers, 39 bits. Framebuffer E0000000, 32 MB. */
    g_am = 0x7ffffff000ull;
    memset(&in, 0, sizeof in);
    in.count     = 10;
    in.phys_bits = 39;
    in.def_type  = MTRR_UC;
    in.var[0]    = var(0, 0x200000000ull, MTRR_WB);
    in.var[1]    = var(0x200000000ull, 0x20000000ull, MTRR_WB);
    in.var[2]    = var(0xE0000000ull, 0x20000000ull, MTRR_UC);
    in.var[3]    = var(0xDC000000ull, 0x4000000ull, MTRR_UC);
    in.var[4]    = var(0xDB000000ull, 0x1000000ull, MTRR_UC);
    in.var[5]    = var(0x21FE00000ull, 0x200000ull, MTRR_UC);
    int rc       = mtrr_plan_wc(&in, 0xE0000000ull, 0x2000000ull, &out, &used);
    CHECK(rc == MTRR_PLAN_DONE);
    printf("  test machine pattern: %d, %d of 10 registers\n", rc, used);
    if (rc == MTRR_PLAN_DONE) {
        /* Cross-check over 0..9 GB in 1 MB steps: everywhere except the
         * framebuffer the same type as before, never undefined (-1). */
        int bad = 0;
        for (uint64_t a = 0; a < 0x240000000ull; a += 0x100000) {
            int before = type_at(&in, a, MTRR_UC), after = type_at(&out, a, MTRR_UC);
            int want   = (a >= 0xE0000000ull && a < 0xE2000000ull) ? MTRR_WC : before;
            if (after != want && bad++ < 5)
                fprintf(stderr, "  %09llx: before %d, after %d, expected %d\n", (unsigned long long) a, before,
                        after, want);
        }
        CHECK(bad == 0);
    }
    /* cross-check the default-WB layout too (SeaBIOS with 2 GB) */
    g_am = 0xffffff000ull;
    memset(&in, 0, sizeof in);
    in.count     = 8;
    in.phys_bits = 36;
    in.def_type  = MTRR_WB;
    in.var[0]    = var(0x80000000ull, 0x80000000ull, MTRR_UC);
    CHECK(mtrr_plan_wc(&in, 0xFD000000ull, 0x1000000ull, &out, &used) == MTRR_PLAN_DONE);
    {
        int bad = 0;
        for (uint64_t a = 0; a < 0x100000000ull; a += 0x100000) {
            int before = type_at(&in, a, MTRR_WB), after = type_at(&out, a, MTRR_WB);
            int want   = (a >= 0xFD000000ull && a < 0xFE000000ull) ? MTRR_WC : before;
            bad += after != want;
        }
        CHECK(bad == 0);
    }

    if (failures) {
        fprintf(stderr, "mtrr_test: %d errors\n", failures);
        return 1;
    }
    printf("mtrr_test: ok\n");
    return 0;
}
