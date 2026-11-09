/* Write-combining for the OVL's VBE framebuffer (Open Watcom).
 *
 * Under DOS/4GW the OVL runs in ring 0 without paging, so the memory type
 * depends on the MTRRs alone. mtrr_plan.c computes the new layout (split
 * the UC range around the framebuffer, add WC); this file writes it using
 * Intel's procedure (SDM vol. 3, "MTRR Considerations in MP Systems", for
 * one core): interrupts off, cache off (CR0.CD), WBINVD, MTRRs off, write,
 * WBINVD, MTRRs on, cache on. wc_dos_off() writes the saved original
 * layout back the same way. Nothing of this survives a reboot.
 *
 * Ring 3 under a paging DPMI host (HDPMI32i, needed by SBEMU): PAT
 * instead of MTRRs. The MTRR procedure through the call gate of r3_dos.c
 * (cache off, WBINVD, MTRRs, ring 0 under the host's paging) froze an
 * i7-4790 in about one call of three. With paging, the page table entry
 * selects a PAT entry, and PAT WC wins over MTRR UC. So the host's entries
 * of the LFB mapping (all write-back, PAT entry 0) are pointed at PAT
 * entry 5 (PWT + PAT bit), which nothing else uses, and entry 5 is set to
 * WC: one WRMSR plus a TLB flush through the gate. wc_dos_off() first
 * points the entries back (effective UC again through the MTRR), then
 * restores entry 5 (WT). The crash path only restores entry 5: entries
 * left pointing at it are then WT under MTRR UC, i.e. uncached.
 *
 * Skipped (plain output) when: ring 3 under Windows or under a host
 * without page tables (VDPMI) or with PAE, no call gate, no PAT, PAT entry
 * 5 in use, LFB entries not plain write-back, no MTRRs/no WC, MTRRs
 * disabled, no suitable range, or GLIDULATOR_WC=0. */

#include "../common/mtrr_plan.h"
#include "r3_dos.h"
#include "video.h"

#include "glidulator/platform.h"

unsigned get_cs(void);
#pragma aux get_cs = "mov ax, cs" "movzx eax, ax" value [eax];
unsigned get_cr0(void);
#pragma aux get_cr0 = "mov eax, cr0" value [eax];
void set_cr0(unsigned v);
#pragma aux set_cr0 = "mov cr0, eax" parm [eax];
unsigned get_cr3(void);
#pragma aux get_cr3 = "mov eax, cr3" value [eax];
void set_cr3(unsigned v);
#pragma aux set_cr3 = "mov cr3, eax" parm [eax];
unsigned get_cr4(void);
#pragma aux get_cr4 = "mov eax, cr4" value [eax];
void wbinvd(void);
#pragma aux wbinvd = 0x0f 0x09;
unsigned save_cli(void);
#pragma aux save_cli = "pushfd" "pop eax" "cli" value [eax];
void restore_flags(unsigned f);
#pragma aux restore_flags = "push eax" "popfd" parm [eax];
unsigned eflags_id(void);
#pragma aux eflags_id = \
    "pushfd" "pop eax" "mov ecx, eax" "xor eax, 200000h" "push eax" "popfd" \
    "pushfd" "pop eax" "push ecx" "popfd" "xor eax, ecx" value [eax] modify [ecx];
void cpuid4(unsigned leaf, unsigned *out);
#pragma aux cpuid4 = \
    "push ebx" 0x0f 0xa2 "mov [edi], eax" "mov [edi+4], ebx" "mov [edi+8], ecx" \
    "mov [edi+12], edx" "pop ebx" parm [eax] [edi] modify [eax ecx edx];
void rdmsr2(unsigned msr, unsigned *lo, unsigned *hi);
#pragma aux rdmsr2 = 0x0f 0x32 "mov [esi], eax" "mov [edi], edx" parm [ecx] [esi] [edi] modify [eax edx];
void wrmsr2(unsigned msr, unsigned lo, unsigned hi);
#pragma aux wrmsr2 = 0x0f 0x30 parm [ecx] [eax] [edx];

#define CR0_PG 0x80000000u
#define CR0_CD 0x40000000u
#define CR0_NW 0x20000000u
#define CR4_PAE 0x20u
#define MSR_PAT     0x277
#define MSR_MTRRCAP 0xfe
#define MSR_DEFTYPE 0x2ff
#define MSR_VAR0    0x200

static mtrr_state g_saved;
static unsigned   g_def_lo, g_def_hi;
static int        g_on;
static int        g_ring3;   /* MSRs through the call gate (r3_dos.c) */
static uint32_t   g_r3_cr3, g_r3_cr4;
static int        g_pat_on;  /* ring 3: LFB entries point at PAT entry 5 = WC */
static uint32_t   g_pat_lin, g_pat_size;
static uint64_t   g_pat_orig;

#define PTE_PWT     0x08u
#define PTE_PCD     0x10u
#define PTE_PAT     0x80u
#define PAT_SHIFT5  40
#define PAT_WC      0x01u
#define PAT_WT      0x04u

/* Windows in enhanced mode: INT 2Fh 160Ah -> AX 0, CX 3 (as smp_dos.c) */
static unsigned windows_mode(void);
#pragma aux windows_mode = "mov ax, 160ah" "xor ecx, ecx" "int 2fh" "test ax, ax" "jz short win_ok" "xor ecx, ecx" \
    "win_ok:" "movzx eax, cx" value [eax] modify [ebx ecx edx];

static uint64_t rdmsr64(unsigned msr) {
    unsigned lo = 0, hi = 0;
    if (g_ring3) {
        uint32_t l = 0, h = 0;
        r3_gate_rdmsr(msr, &l, &h);
        lo = l;
        hi = h;
    } else {
        rdmsr2(msr, &lo, &hi);
    }
    return ((uint64_t) hi << 32) | lo;
}

static void wrmsr64(unsigned msr, uint64_t v) {
    wrmsr2(msr, (unsigned) v, (unsigned) (v >> 32));
}

static void apply(const mtrr_state *s) {
    unsigned flags = save_cli();
    unsigned cr0   = get_cr0();
    set_cr0((cr0 | CR0_CD) & ~CR0_NW);
    wbinvd();
    if (cr0 & CR0_PG)
        set_cr3(get_cr3()); /* flush the TLB */
    wrmsr2(MSR_DEFTYPE, g_def_lo & ~0x800u, g_def_hi); /* MTRRs off */
    for (int i = 0; i < s->count; i++) {
        wrmsr64(MSR_VAR0 + 2 * i, s->var[i].base);
        wrmsr64(MSR_VAR0 + 2 * i + 1, s->var[i].mask);
    }
    wbinvd();
    if (cr0 & CR0_PG)
        set_cr3(get_cr3());
    wrmsr2(MSR_DEFTYPE, g_def_lo, g_def_hi); /* back on */
    set_cr0(cr0);
    restore_flags(flags);
}

#ifdef GLIDULATOR_TRACE
static char *put_s(char *p, const char *s) {
    while (*s)
        *p++ = *s++;
    return p;
}

static char *put_hex(char *p, unsigned v) {
    for (int i = 28; i >= 0; i -= 4)
        *p++ = "0123456789ABCDEF"[(v >> i) & 15];
    return p;
}

static char *put_dec(char *p, unsigned v) {
    char t[12];
    int  k = 0;
    do
        t[k++] = (char) ('0' + v % 10);
    while ((v /= 10) && k < 11);
    while (k)
        *p++ = t[--k];
    return p;
}

static void log_wc(const char *what, unsigned phys, unsigned kb, int used, int count) {
    char  line[160];
    char *p = put_s(line, "Write-Combining: ");
    p       = put_s(p, what);
    if (kb) {
        p = put_s(p, ", ");
        p = put_hex(p, phys);
        p = put_s(p, " + ");
        p = put_dec(p, kb);
        p = put_s(p, " KB");
    }
    if (count) {
        p = put_s(p, ", MTRRs ");
        p = put_dec(p, (unsigned) used);
        p = put_s(p, " of ");
        p = put_dec(p, (unsigned) count);
    }
    *p++ = '\n';
    *p   = 0;
    platform_log(line);
}
/* Log the variable MTRRs (to learn new firmware layouts). */
static void log_mtrrs(const mtrr_state *s, unsigned def_lo) {
    char  line[96];
    char *p = put_s(line, "MTRRdefType ");
    p       = put_hex(p, def_lo);
    *p++    = '\n';
    *p      = 0;
    platform_log(line);
    for (int i = 0; i < s->count; i++) {
        if (!(s->var[i].mask & 0x800))
            continue;
        p    = put_s(line, "  MTRR");
        p    = put_dec(p, (unsigned) i);
        p    = put_s(p, ": base ");
        p    = put_hex(p, (unsigned) (s->var[i].base >> 32));
        p    = put_hex(p, (unsigned) s->var[i].base);
        p    = put_s(p, " mask ");
        p    = put_hex(p, (unsigned) (s->var[i].mask >> 32));
        p    = put_hex(p, (unsigned) s->var[i].mask);
        *p++ = '\n';
        *p   = 0;
        platform_log(line);
    }
}
#    define LOG_WC(w, a, k, u, c) log_wc(w, a, k, u, c)
#else
#    define LOG_WC(w, a, k, u, c) ((void) 0)
#endif

/* Ring 3: the host's page tables and a call gate to ring 0 (r3_dos.c).
 * The first MSR access through the gate runs behind the DPMI exception
 * handlers; 1 = usable. */
static int ring3_open(void) {
    uint32_t cr0 = 0, cr3 = 0, cr4 = 0, lo, hi;
    if (g_ring3)
        return 1;
    if (windows_mode() == 3) {
        LOG_WC("off (ring 3 under Windows)", 0, 0, 0, 0);
        return 0;
    }
    if (!r3_guard_begin()) {
        LOG_WC("off (ring 3, no DPMI exception handling)", 0, 0, 0, 0);
        return 0;
    }
    int ok = r3_try_read_cr(0, &cr0) && r3_try_read_cr(3, &cr3) && r3_try_read_cr(4, &cr4);
    if (!ok || !cr3 || !(cr0 & CR0_PG)) {
        r3_guard_end();
        LOG_WC("off (ring 3, host without readable page tables, e.g. VDPMI)", 0, 0, 0, 0);
        return 0;
    }
    if (cr4 & CR4_PAE) {
        r3_guard_end();
        LOG_WC("off (ring 3, host uses PAE paging)", 0, 0, 0, 0);
        return 0;
    }
    if (!r3_gate_open(cr3) || !r3_gate_rdmsr(MSR_MTRRCAP, &lo, &hi)) {
        r3_guard_end();
        LOG_WC("off (ring 3, no call gate to ring 0)", 0, 0, 0, 0);
        return 0;
    }
    r3_guard_end();
    g_r3_cr3 = cr3;
    g_r3_cr4 = cr4;
    g_ring3  = 1;
    return 1;
}

#ifdef GLIDULATOR_TRACE
/* Ring 3: the memory type also depends on the host's page table entry of
 * the LFB mapping (PWT/PCD/PAT select a PAT entry; UC there overrides WC
 * of the MTRR). */
static void log_lfb_entry(uint32_t phys) {
    uint32_t lin = vid_dpmi_mapped_lin(phys);
    char     line[128], *p;
    int      large = 0;
    if (!lin)
        return;
    uint32_t e  = r3_page_entry(g_r3_cr3, lin, &large);
    uint64_t pat = rdmsr64(MSR_PAT);
    unsigned idx = ((e >> 3) & 1) | (((e >> 4) & 1) << 1) | (((e >> (large ? 12 : 7)) & 1) << 2);
    p    = put_s(line, "Write-Combining: LFB at ");
    p    = put_hex(p, lin);
    p    = put_s(p, large ? ", PDE " : ", PTE ");
    p    = put_hex(p, e);
    p    = put_s(p, ", PAT ");
    p    = put_hex(p, (unsigned) (pat >> 32));
    p    = put_hex(p, (unsigned) pat);
    p    = put_s(p, " entry ");
    p    = put_dec(p, idx);
    p    = put_s(p, " type ");
    p    = put_dec(p, (unsigned) (pat >> (8 * idx)) & 7);
    *p++ = '\n';
    *p   = 0;
    platform_log(line);
}
#endif

/* Ring 3: PAT entry 5 = WC for the LFB mapping (see the top). */
static int ring3_on(uint32_t phys, uint32_t need) {
    unsigned r[4];
    if (!ring3_open())
        return -1;
    cpuid4(1, r);
    if (!(r[3] & (1u << 16))) {
        LOG_WC("off (ring 3, CPU without PAT)", 0, 0, 0, 0);
        return -1;
    }
    uint32_t lin = vid_dpmi_mapped_lin(phys);
    if (!lin) {
        LOG_WC("off (ring 3, LFB mapping unknown)", 0, 0, 0, 0);
        return -1;
    }
    uint64_t pat  = rdmsr64(MSR_PAT);
    unsigned pat5 = (unsigned) (pat >> PAT_SHIFT5) & 0xff;
    if (pat5 != PAT_WT && pat5 != PAT_WC) {
        LOG_WC("off (ring 3, PAT entry 5 in use)", 0, 0, 0, 0);
        return -1;
    }
    if (!r3_pte_change(g_r3_cr3, lin, need, PTE_PWT | PTE_PCD | PTE_PAT, 0, PTE_PWT | PTE_PAT)) {
        LOG_WC("off (ring 3, LFB not mapped plain write-back)", phys, need / 1024, 0, 0);
        return -1;
    }
    /* entry 5 was WT if a previous run did not end cleanly */
    g_pat_orig     = (pat & ~((uint64_t) 0xff << PAT_SHIFT5)) | ((uint64_t) PAT_WT << PAT_SHIFT5);
    uint64_t newp  = (pat & ~((uint64_t) 0xff << PAT_SHIFT5)) | ((uint64_t) PAT_WC << PAT_SHIFT5);
    if (!r3_gate_wrmsr_flush(MSR_PAT, (uint32_t) newp, (uint32_t) (newp >> 32))) {
        r3_pte_change(g_r3_cr3, lin, need, PTE_PWT | PTE_PCD | PTE_PAT, PTE_PWT | PTE_PAT, 0);
        LOG_WC("off (ring 3, call gate refused)", 0, 0, 0, 0);
        return -1;
    }
    g_pat_lin  = lin;
    g_pat_size = need;
    g_pat_on   = 1;
    g_on       = 1;
    LOG_WC("on (ring 3: PAT entry 5 through a call gate)", phys, need / 1024, 0, 0);
#ifdef GLIDULATOR_TRACE
    log_lfb_entry(phys);
#endif
    return 1;
}

static void ring3_off(int crash) {
    if (!crash && !r3_pte_change(g_r3_cr3, g_pat_lin, g_pat_size, PTE_PWT | PTE_PCD | PTE_PAT, PTE_PWT | PTE_PAT, 0))
        LOG_WC("LFB entries changed meanwhile, left as they are", 0, 0, 0, 0);
    r3_gate_wrmsr_flush(MSR_PAT, (uint32_t) g_pat_orig, (uint32_t) (g_pat_orig >> 32));
    g_pat_on = 0;
}

int VID_CALL wc_dos_on(uint32_t phys, uint32_t need, uint32_t vram) {
    unsigned r[4];
    if (g_on)
        return 1;
    if (get_cs() & 3)
        return ring3_on(phys, need);
    if (!(eflags_id() & 0x200000u)) {
        LOG_WC("off (no CPUID)", 0, 0, 0, 0);
        return -1;
    }
    cpuid4(1, r);
    if (!(r[3] & (1u << 12)) || !(r[3] & (1u << 5))) {
        LOG_WC("off (CPU without MTRRs)", 0, 0, 0, 0);
        return -1;
    }
    uint64_t cap = rdmsr64(MSR_MTRRCAP);
    int      n   = (int) (cap & 0xff);
    if (!(cap & 0x400) || n <= 0) {
        LOG_WC("off (CPU without WC MTRR type)", 0, 0, 0, 0);
        return -1;
    }
    if (n > MTRR_MAX_VAR)
        n = MTRR_MAX_VAR;
    uint64_t def = rdmsr64(MSR_DEFTYPE);
    g_def_lo     = (unsigned) def;
    g_def_hi     = (unsigned) (def >> 32);
    if (!(g_def_lo & 0x800)) {
        LOG_WC("off (MTRRs disabled)", 0, 0, 0, 0);
        return -1;
    }
    g_saved.count     = n;
    g_saved.def_type  = (int) (g_def_lo & 0xff);
    g_saved.phys_bits = 36;
    cpuid4(0x80000000u, r);
    if (r[0] >= 0x80000008u) {
        cpuid4(0x80000008u, r);
        if ((r[0] & 0xff) >= 32)
            g_saved.phys_bits = (int) (r[0] & 0xff);
    }
    for (int i = 0; i < n; i++) {
        g_saved.var[i].base = rdmsr64(MSR_VAR0 + 2 * i);
        g_saved.var[i].mask = rdmsr64(MSR_VAR0 + 2 * i + 1);
    }
#ifdef GLIDULATOR_TRACE
    log_mtrrs(&g_saved, g_def_lo);
#endif
    uint64_t size = mtrr_wc_size(phys, need, vram);
    if (!size) {
        LOG_WC("off (framebuffer not aligned)", phys, need / 1024, 0, 0);
        return -1;
    }
    static mtrr_state plan;
    int               used = 0;
    int               rc   = mtrr_plan_wc(&g_saved, phys, size, &plan, &used);
    if (rc == MTRR_PLAN_NOROOM && !(get_cr4() & CR4_PAE)) {
        /* without PAE nothing above 4 GB is reachable: leave those ranges
         * out while we run (g_saved restores them) */
        static mtrr_state low;
        if (mtrr_clip_4g(&g_saved, &low)) {
            rc = mtrr_plan_wc(&low, phys, size, &plan, &used);
            if (rc == MTRR_PLAN_DONE)
                LOG_WC("ranges above 4 GB left out (no PAE)", 0, 0, 0, 0);
        }
    }
    if (rc == MTRR_PLAN_ALREADY) {
        LOG_WC("already on", phys, (unsigned) (size / 1024), 0, 0);
        return 0;
    }
    if (rc != MTRR_PLAN_DONE) {
        LOG_WC(rc == MTRR_PLAN_NOROOM ? "off (too few free MTRRs)" : "off (range overlaps non-UC)", phys,
               (unsigned) (size / 1024), 0, n);
        return -1;
    }
    apply(&plan);
    g_on = 1;
    LOG_WC("on", phys, (unsigned) (size / 1024), used, n);
    return 1;
}

/* Emergency (diag_dos.c): like wc_dos_off, without the video layer's calling convention */
void wc_dos_off_crash(void) {
    if (g_on) {
        if (g_pat_on)
            ring3_off(1);
        else
            apply(&g_saved);
        g_on = 0;
    }
}

void VID_CALL wc_dos_off(void) {
    if (!g_on)
        return;
    if (g_pat_on) {
        ring3_off(0);
        g_on = 0;
        LOG_WC("off (LFB entries and PAT entry 5 restored)", 0, 0, 0, 0);
        return;
    }
    apply(&g_saved);
    g_on = 0;
    LOG_WC("off (original MTRRs restored)", 0, 0, 0, 0);
}
