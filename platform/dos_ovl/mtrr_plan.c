/* See mtrr_plan.h. No C runtime (runs in the OVL). */

#include "mtrr_plan.h"

#define VALID 0x800u

typedef struct {
    uint64_t base, size;
    int      type;
} range;

static uint64_t addr_mask(int phys_bits) {
    return ((phys_bits >= 64 ? 0 : ((uint64_t) 1 << phys_bits)) - 1) & ~(uint64_t) 0xfff;
}

/* MTRR -> range; 0 for a non-contiguous mask */
static int to_range(const mtrr_var *v, int phys_bits, range *r) {
    uint64_t am   = addr_mask(phys_bits);
    uint64_t mask = v->mask & am;
    uint64_t size = (~mask & am) + 0x1000;
    if (size & (size - 1))
        return 0;
    r->base = v->base & am;
    r->size = size;
    r->type = (int) (v->base & 0xff);
    return 1;
}

static mtrr_var from_range(uint64_t base, uint64_t size, int type, int phys_bits) {
    uint64_t am = addr_mask(phys_bits);
    mtrr_var v;
    v.base = (base & am) | (uint64_t) type;
    v.mask = (~(size - 1) & am) | VALID;
    return v;
}

/* split [a, b) into aligned power-of-two blocks, append as UC */
static int add_pieces(range *list, int n, int cap, uint64_t a, uint64_t b) {
    while (a < b) {
        uint64_t s = 0x1000;
        while ((a & (s * 2 - 1)) == 0 && a + s * 2 <= b)
            s *= 2;
        if (n >= cap)
            return -1;
        list[n].base = a;
        list[n].size = s;
        list[n].type = MTRR_UC;
        n++;
        a += s;
    }
    return n;
}

static int write_out(const mtrr_state *in, const range *list, int n, uint64_t base, uint64_t size,
                     mtrr_state *out, int *used) {
    int cap = in->count;
    if (n + 1 > cap)
        return MTRR_PLAN_NOROOM;
    out->count     = cap;
    out->phys_bits = in->phys_bits;
    out->def_type  = in->def_type;
    for (int i = 0; i < cap; i++) {
        out->var[i].base = 0;
        out->var[i].mask = 0;
    }
    for (int i = 0; i < n; i++)
        out->var[i] = from_range(list[i].base, list[i].size, list[i].type, in->phys_bits);
    out->var[n] = from_range(base, size, MTRR_WC, in->phys_bits);
    *used       = n + 1;
    return MTRR_PLAN_DONE;
}

/* Default type UC: remove the UC hole H (a UC range that contains the
 * whole framebuffer) from all other ranges. */
static int plan_uc_default(const mtrr_state *in, uint64_t base, uint64_t size, mtrr_state *out, int *used) {
    range    all[MTRR_MAX_VAR], list[MTRR_MAX_VAR * 2];
    int      na = 0, n = 0, h = -1;
    uint64_t end = base + size;
    for (int i = 0; i < in->count; i++) {
        if (!(in->var[i].mask & VALID))
            continue;
        if (!to_range(&in->var[i], in->phys_bits, &all[na]))
            return MTRR_PLAN_CONFLICT;
        if (all[na].type == MTRR_UC && all[na].base <= base && all[na].base + all[na].size >= end &&
            (h < 0 || all[na].size > all[h].size))
            h = na; /* largest UC hole around the framebuffer */
        na++;
    }
    if (h < 0)
        return MTRR_PLAN_CONFLICT;
    uint64_t hb = all[h].base, he = all[h].base + all[h].size;
    for (int i = 0; i < na; i++) {
        uint64_t b = all[i].base, e = all[i].base + all[i].size;
        if (e <= hb || b >= he) {
            if (n >= MTRR_MAX_VAR * 2)
                return MTRR_PLAN_NOROOM;
            list[n++] = all[i]; /* outside H: unchanged */
        } else if (b >= hb && e <= he) {
            continue; /* inside H: dropped, the default type UC applies there */
        } else if (all[i].type == MTRR_UC) {
            list[n++] = all[i]; /* larger than H, UC: may stay */
        } else {
            /* larger than H, not UC (the large WB range): without H */
            int t = all[i].type, k = n;
            n     = add_pieces(list, n, MTRR_MAX_VAR * 2, b, hb);
            if (n >= 0)
                n = add_pieces(list, n, MTRR_MAX_VAR * 2, he, e);
            if (n < 0)
                return MTRR_PLAN_NOROOM;
            for (; k < n; k++)
                list[k].type = t;
        }
    }
    return write_out(in, list, n, base, size, out, used);
}

int mtrr_plan_wc(const mtrr_state *in, uint64_t base, uint64_t size, mtrr_state *out, int *used) {
    range list[MTRR_MAX_VAR * 2];
    int   n = 0, cap = in->count;
    if (size < 0x1000 || (size & (size - 1)) || (base & (size - 1)) || cap > MTRR_MAX_VAR)
        return MTRR_PLAN_BADARGS;
    uint64_t end = base + size;
    int      conflict = 0;
    for (int i = 0; i < cap; i++) {
        range r;
        if (!(in->var[i].mask & VALID))
            continue;
        if (!to_range(&in->var[i], in->phys_bits, &r))
            return MTRR_PLAN_CONFLICT;
        uint64_t rend = r.base + r.size;
        if (rend <= base || r.base >= end) {
            if (n >= MTRR_MAX_VAR * 2)
                return MTRR_PLAN_NOROOM;
            list[n++] = r; /* untouched */
            continue;
        }
        if (r.type == MTRR_WC && r.base <= base && rend >= end)
            return MTRR_PLAN_ALREADY;
        if (r.type != MTRR_UC) {
            conflict = 1;
            continue;
        }
        /* rebuild the UC range without the framebuffer */
        n = add_pieces(list, n, MTRR_MAX_VAR * 2, r.base, base > r.base ? base : r.base);
        if (n >= 0)
            n = add_pieces(list, n, MTRR_MAX_VAR * 2, end < rend ? end : rend, rend);
        if (n < 0)
            return MTRR_PLAN_NOROOM;
    }
    if (conflict) /* a non-UC range overlaps the framebuffer */
        return in->def_type == MTRR_UC ? plan_uc_default(in, base, size, out, used) : MTRR_PLAN_CONFLICT;
    return write_out(in, list, n, base, size, out, used);
}

uint64_t mtrr_wc_size(uint64_t phys, uint64_t need, uint64_t vram) {
    uint64_t s = 0x1000;
    while (s < need)
        s *= 2;
    if (phys & (s - 1))
        return 0;
    while (s * 2 <= vram && !(phys & (s * 2 - 1)))
        s *= 2;
    return s;
}
