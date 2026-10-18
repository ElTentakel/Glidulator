/* Write-combining for the OVL's VBE framebuffer (Open Watcom, ring 0).
 *
 * Under DOS/4GW the OVL runs in ring 0 without paging, so the memory type
 * depends on the MTRRs alone. mtrr_plan.c computes the new layout (split
 * the UC range around the framebuffer, add WC); this file writes it using
 * Intel's procedure (SDM vol. 3, "MTRR Considerations in MP Systems", for
 * one core): interrupts off, cache off (CR0.CD), WBINVD, MTRRs off, write,
 * WBINVD, MTRRs on, cache on. wc_dos_off() writes the saved original
 * layout back the same way. Nothing of this survives a reboot.
 *
 * Skipped (plain output) when: not ring 0 (Windows DOS box), no MTRRs/no
 * WC, MTRRs disabled, no suitable range, or GLIDULATOR_WC=0. */

#include "../common/mtrr_plan.h"
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
#define MSR_MTRRCAP 0xfe
#define MSR_DEFTYPE 0x2ff
#define MSR_VAR0    0x200

static mtrr_state g_saved;
static unsigned   g_def_lo, g_def_hi;
static int        g_on;

static uint64_t rdmsr64(unsigned msr) {
    unsigned lo, hi;
    rdmsr2(msr, &lo, &hi);
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

int VID_CALL wc_dos_on(uint32_t phys, uint32_t need, uint32_t vram) {
    unsigned r[4];
    if (g_on)
        return 1;
    if (get_cs() & 3) {
        LOG_WC("off (not ring 0, e.g. Windows DOS box)", 0, 0, 0, 0);
        return -1;
    }
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
    rdmsr2(MSR_DEFTYPE, &g_def_lo, &g_def_hi);
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
        apply(&g_saved);
        g_on = 0;
    }
}

void VID_CALL wc_dos_off(void) {
    if (!g_on)
        return;
    apply(&g_saved);
    g_on = 0;
    LOG_WC("off (original MTRRs restored)", 0, 0, 0, 0);
}
