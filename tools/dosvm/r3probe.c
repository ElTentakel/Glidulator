/* Multi-core from ring 3: can the OVL start the other cores itself while a
 * DPMI host (HDPMI32i for SBEMU, VDPMI) runs it in ring 3? The other cores
 * (APs) do not belong to the host; after INIT/SIPI they run our trampoline
 * in ring 0. They only need the host's page directory (CR3) to see the
 * same linear addresses as the main core. Checked here:
 *
 *   - CPL, selector bases/limits, DPMI version
 *   - CR0/CR3/CR4, RDMSR, a CR4 write of the same value: does the host
 *     emulate them for ring 3 or raise the exception (DPMI 0203h handler)?
 *   - page walk from CR3 (page directory and tables mapped via DPMI 0800h):
 *     does the translation of static data, heap, stack and DOS memory hit
 *     the right physical page (read back through a second mapping)?
 *   - local APIC via DPMI 0800h at FEE00000h (xAPIC mode)
 *   - with argument AP: start the APs with the trampoline of the OVL
 *     (platform/dos_ovl/ap_tramp.S), an own flat ring-0 GDT and the host's
 *     CR3; every AP writes to heap memory and calls code of this program,
 *     then halts. INIT puts them back to wait-for-SIPI.
 *
 * Output appended to R3PROBE.TXT (file closed after every line, so a hang
 * leaves everything up to it). Built by tools/dosvm/run.sh (R3PROBE=...). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <i86.h>

#include "ap_tramp.h"

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

static void out(const char *fmt, ...) {
    va_list a;
    FILE   *f = fopen("R3PROBE.TXT", "a");
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fclose(f);
}

/* ---- Exceptions: skip the faulting instruction (g_skip bytes) ---- */

static volatile u32 g_skip, g_fault, g_ferr;
static u16          g_ds;
static u32          g_old6[2], g_old13[2], g_old14[2];

/* DPMI 32-bit exception frame after push ds/eax: [esp] eax, [esp+4] ds,
 * [esp+8] return EIP, [esp+12] return CS, [esp+16] error code, [esp+20]
 * faulting EIP. Unexpected faults (g_skip 0) go to the previous handler. */
static void __declspec(naked) exc6(void) {
    _asm {
        push ds
        push eax
        mov ax, word ptr cs:g_ds
        mov ds, ax
        cmp g_skip, 0
        je chain6
        mov g_fault, 6
        mov eax, g_skip
        add [esp+20], eax
        pop eax
        pop ds
        retf
    chain6:
        pop eax
        pop ds
        jmp fword ptr cs:g_old6
    }
}
static void __declspec(naked) exc13(void) {
    _asm {
        push ds
        push eax
        mov ax, word ptr cs:g_ds
        mov ds, ax
        cmp g_skip, 0
        je chain13
        mov g_fault, 13
        mov eax, [esp+16]
        mov g_ferr, eax
        mov eax, g_skip
        add [esp+20], eax
        pop eax
        pop ds
        retf
    chain13:
        pop eax
        pop ds
        jmp fword ptr cs:g_old13
    }
}
static void __declspec(naked) exc14(void) {
    _asm {
        push ds
        push eax
        mov ax, word ptr cs:g_ds
        mov ds, ax
        cmp g_skip, 0
        je chain14
        mov g_fault, 14
        mov eax, [esp+16]
        mov g_ferr, eax
        mov eax, g_skip
        add [esp+20], eax
        pop eax
        pop ds
        retf
    chain14:
        pop eax
        pop ds
        jmp fword ptr cs:g_old14
    }
}

static unsigned get_cs(void);
#pragma aux get_cs = "xor eax, eax" "mov ax, cs" value [eax];
static unsigned get_ds(void);
#pragma aux get_ds = "xor eax, eax" "mov ax, ds" value [eax];
static unsigned lsl(unsigned sel);
#pragma aux lsl = "xor eax, eax" "lsl eax, edx" parm [edx] value [eax];
static unsigned rd_cr0(void);
#pragma aux rd_cr0 = "xor eax, eax" 0x0f 0x20 0xc0 value [eax];
static unsigned rd_cr3(void);
#pragma aux rd_cr3 = "xor eax, eax" 0x0f 0x20 0xd8 value [eax];
static unsigned rd_cr4(void);
#pragma aux rd_cr4 = "xor eax, eax" 0x0f 0x20 0xe0 value [eax];
static void wr_cr4(unsigned v);
#pragma aux wr_cr4 = 0x0f 0x22 0xe0 parm [eax];
static void rdmsr2(unsigned msr, unsigned *lo, unsigned *hi);
#pragma aux rdmsr2 = "xor eax, eax" "xor edx, edx" 0x0f 0x32 "mov [esi], eax" "mov [edi], edx" parm [ecx] [esi] [edi] modify [eax edx];
static unsigned rd_mem(const void *p);
#pragma aux rd_mem = "xor eax, eax" 0x8b 0x02 parm [edx] value [eax]; /* mov eax, [edx] */
static void sse_op(void);
#pragma aux sse_op = 0x0f 0x57 0xc0; /* xorps xmm0, xmm0 */
static void sgdt(void *p);
#pragma aux sgdt = 0x0f 0x01 0x00 parm [eax];
static void cpuid4(unsigned leaf, unsigned sub, unsigned *r);
#pragma aux cpuid4 = "push ebx" 0x0f 0xa2 "mov [edi], eax" "mov [edi+4], ebx" "mov [edi+8], ecx" "mov [edi+12], edx" "pop ebx" parm [eax] [ecx] [edi] modify [eax ecx edx];
static void rdtsc2(u32 *v);
#pragma aux rdtsc2 = 0x0f 0x31 "mov [ecx], eax" "mov [ecx+4], edx" parm [ecx] modify [eax edx];
static void cpu_pause(void);
#pragma aux cpu_pause = 0xf3 0x90;
static void park(void);
#pragma aux park = "again:" "cli" "hlt" "jmp short again" aborts;

/* run one guarded instruction; returns the vector (0 = no fault) */
#define TRY(len, stmt) (g_fault = 0, g_skip = (len), (stmt), g_skip = 0, g_fault)

/* ---- DPMI ---- */

static int dpmi_get_exc(unsigned n, u32 *old) {
    union REGS r;
    memset(&r, 0, sizeof r);
    r.w.ax = 0x0202;
    r.h.bl = (u8) n;
    int386(0x31, &r, &r);
    old[0] = r.x.edx;
    old[1] = r.w.cx;
    return !r.w.cflag;
}

static int dpmi_set_exc(unsigned n, void (*fn)(void)) {
    union REGS r;
    memset(&r, 0, sizeof r);
    r.w.ax  = 0x0203;
    r.h.bl  = (u8) n;
    r.w.cx  = (u16) get_cs();
    r.x.edx = (u32) fn;
    int386(0x31, &r, &r);
    return !r.w.cflag;
}

static void dpmi_restore_exc(unsigned n, const u32 *old) {
    union REGS r;
    memset(&r, 0, sizeof r);
    r.w.ax  = 0x0203;
    r.h.bl  = (u8) n;
    r.w.cx  = (u16) old[1];
    r.x.edx = old[0];
    int386(0x31, &r, &r);
}

static u32 sel_base(unsigned sel) {
    union REGS r;
    memset(&r, 0, sizeof r);
    r.w.ax = 0x0006;
    r.w.bx = (u16) sel;
    int386(0x31, &r, &r);
    return r.w.cflag ? 0xffffffffu : ((u32) r.w.cx << 16) | r.w.dx;
}

static void *map_phys(u32 phys, u32 size) {
    union REGS r;
    memset(&r, 0, sizeof r);
    r.w.ax = 0x0800;
    r.w.bx = (u16) (phys >> 16);
    r.w.cx = (u16) phys;
    r.w.si = (u16) (size >> 16);
    r.w.di = (u16) size;
    int386(0x31, &r, &r);
    if (r.w.cflag)
        return NULL;
    return (void *) (((u32) r.w.bx << 16) | r.w.cx);
}

static unsigned dos_alloc(unsigned paras, unsigned *sel) {
    union REGS r;
    memset(&r, 0, sizeof r);
    r.w.ax = 0x0100;
    r.w.bx = (u16) paras;
    int386(0x31, &r, &r);
    if (r.w.cflag)
        return 0;
    *sel = r.w.dx;
    return r.w.ax;
}

static void dos_free(unsigned sel) {
    union REGS r;
    memset(&r, 0, sizeof r);
    r.w.ax = 0x0101;
    r.w.dx = (u16) sel;
    int386(0x31, &r, &r);
}

/* ---- Page walk ---- */

static u32  g_cr3, g_cr4;
static int  g_cr4_ok;
static u32 *g_pd;
static struct {
    u32  phys;
    u32 *p;
} g_pt[32];
static int g_npt;

static u32 *map_table(u32 phys) {
    for (int i = 0; i < g_npt; i++)
        if (g_pt[i].phys == phys)
            return g_pt[i].p;
    u32 *p = (u32 *) map_phys(phys, 4096);
    if (p && g_npt < 32) {
        g_pt[g_npt].phys = phys;
        g_pt[g_npt].p    = p;
        g_npt++;
    }
    return p;
}

/* linear -> physical; *pde/*pte get the entries (pte 0 for a 4 MB page) */
static int translate(u32 lin, u32 *phys, u32 *pde, u32 *pte) {
    u32 d = g_pd[lin >> 22];
    *pde  = d;
    *pte  = 0;
    if (!(d & 1))
        return 0;
    if (d & 0x80) {
        *phys = (d & 0xffc00000u) | (lin & 0x3fffffu);
        return 1;
    }
    u32 *pt = map_table(d & 0xfffff000u);
    if (!pt)
        return 0;
    u32 t = pt[(lin >> 12) & 1023];
    *pte  = t;
    if (!(t & 1))
        return 0;
    *phys = (t & 0xfffff000u) | (lin & 0xfff);
    return 1;
}

/* translate, then write through a second mapping of the physical page and
 * read back through the original address (code: compare only) */
static void check_lin(const char *what, volatile u32 *v, int write) {
    u32 phys, pde, pte;
    if (!translate((u32) v, &phys, &pde, &pte)) {
        out("  %-12s lin %08x: not present (PDE %08x PTE %08x)\n", what, (u32) v, pde, pte);
        return;
    }
    const char *res;
    u32         orig = *v;
    if (write)
        *v = 0x5a5a1234u ^ (u32) v;
    volatile u32 *alias = (volatile u32 *) map_phys(phys & ~0xfffu, 4096);
    if (!alias) {
        res = (phys < 0x100000 && phys == (u32) v) ? "identity (below 1 MB, no 0800h)" : "0800h refused";
    } else {
        alias = (volatile u32 *) ((u8 *) alias + (phys & 0xfff));
        u32 a = *alias;
        if (write) {
            *alias = 0xc3c3c3c3u;
            res    = (a == (0x5a5a1234u ^ (u32) v) && *v == 0xc3c3c3c3u) ? "OK" : "MISMATCH";
        } else {
            res = a == orig ? "OK (read)" : "MISMATCH";
        }
    }
    out("  %-12s lin %08x -> phys %08x  PDE %08x PTE %08x  %s\n", what, (u32) v, phys, pde, pte, res);
}

/* ---- Timing (TSC against the BIOS tick) ---- */

static u32 g_tsc_per_ms;

static unsigned long long tsc(void) {
    u32 v[2];
    rdtsc2(v);
    return ((unsigned long long) v[1] << 32) | v[0];
}

static void calibrate(void) {
    volatile u32 *tick = (volatile u32 *) 0x46c;
    u32           t    = *tick;
    while (*tick == t)
        ;
    unsigned long long a = tsc();
    t                    = *tick;
    while (*tick - t < 2)
        ;
    g_tsc_per_ms = (u32) ((tsc() - a) / 110);
}

static void wait_us(u32 us) {
    unsigned long long t0 = tsc(), n = (unsigned long long) g_tsc_per_ms * us / 1000;
    while (tsc() - t0 < n)
        cpu_pause();
}

/* ---- APs ---- */

#define P_BASE   0x100
#define TR_IDT   0x400
#define TR_STUBS 0x200
#define TR_PM32  0x60
#define TR_TGUARD 0x680 /* the trampoline's guard (ap_tramp.S G_*) */
#define TR_GDT   0x6a0 /* own GDT: null, code, data */
#define TR_GUARD 0x700 /* guard code, then its data at TR_GDATA */
#define TR_GDATA 0x780
#define AP_MAX   8
#define AP_STACK 0x4000u

#pragma pack(push, 1)
typedef struct {
    u16 gdt_limit;
    u32 gdt_base;
    u16 pad0;
    u16 idt_limit;
    u32 idt_base;
    u16 pad1;
    u32 jmp_off;
    u16 jmp_sel;
    u16 pad2;
    u16 dsel;
    u16 pad3;
    u32 cr0, cr4, count, max, stack, stsize, entry;
    u32 exc_flag, exc_vec, exc_w[3], exc_esp;
    u32 mc_n, mc[10][4];
    u32 cr3;
} tr_params;
#pragma pack(pop)

static volatile u32 *g_apic;
static volatile u32  g_ap_apic[AP_MAX], g_ap_done[AP_MAX], g_ap_sum[AP_MAX];
static u32          *g_heap_buf; /* malloc'd: shared memory test */

static u32 sum_of(const u32 *p, int n) { /* code of this program, run on the AP */
    u32 s = 0;
    for (int i = 0; i < n; i++)
        s += p[i] * (u32) (i + 1);
    return s;
}

static void __cdecl ap_entry(int idx) {
    unsigned r[4];
    cpuid4(1, 0, r);
    g_ap_apic[idx] = r[1] >> 24;
    g_ap_sum[idx]  = sum_of(g_heap_buf, 1024);
    g_heap_buf[1024 + idx] = 0xa0000000u | idx;
    g_ap_done[idx] = 1;
    park();
}

/* Targets as in the OVL (smp_dos.c): the enabled local APICs of the ACPI
 * MADT except this core, never a broadcast if there is an MADT. Firmware
 * may own an AP (CSMWrap's BIOS proxy is hidden from the MADT). */
static u32 g_tgt[16];
static int g_ntgt = -1; /* -1: no MADT, broadcast */

static int phys_copy(u32 phys, void *dst, u32 len) {
    u32 off = phys & 0xfff;
    u8 *m   = (u8 *) map_phys(phys - off, (off + len + 0xfff) & ~0xfffu);
    if (!m)
        return 0;
    memcpy(dst, m + off, len);
    {   /* DPMI 0801h: one mapping at a time (HDPMI shares linear pages) */
        union REGS r;
        memset(&r, 0, sizeof r);
        r.w.ax = 0x0801;
        r.w.bx = (u16) ((u32) m >> 16);
        r.w.cx = (u16) (u32) m;
        int386(0x31, &r, &r);
    }
    return 1;
}

static void plan_targets(unsigned bsp) {
    static u8 buf[4096];
    u32       ebda = (u32) *(const u16 *) 0x40e << 4, rsdt = 0, hdr[2], ent[64];
    const u8 *rsdp = NULL;
    for (int k = (ebda >= 0x80000 && ebda < 0xa0000) ? 0 : 1; k < 2 && !rsdp; k++) {
        u32 a = k ? 0xe0000 : ebda, end = k ? 0x100000 : ebda + 1024;
        for (; a + 20 <= end && !rsdp; a += 16)
            if (!memcmp((const void *) a, "RSD PTR ", 8)) {
                u8 sum = 0;
                for (int i = 0; i < 20; i++)
                    sum += ((const u8 *) a)[i];
                if (!sum)
                    rsdp = (const u8 *) a;
            }
    }
    if (!rsdp) {
        out("MADT: no RSDP, IPIs by broadcast\n");
        return;
    }
    rsdt = *(const u32 *) (rsdp + 16);
    if (!phys_copy(rsdt, hdr, 8) || hdr[1] < 36 || hdr[1] > sizeof buf || !phys_copy(rsdt, buf, hdr[1]))
        return;
    int nent = 0;
    for (u32 o = 36; o + 4 <= hdr[1] && nent < 64; o += 4)
        ent[nent++] = *(const u32 *) (buf + o);
    for (int i = 0; i < nent; i++) {
        if (!phys_copy(ent[i], hdr, 8) || memcmp(hdr, "APIC", 4) || hdr[1] < 44 || hdr[1] > sizeof buf ||
            !phys_copy(ent[i], buf, hdr[1]))
            continue;
        g_ntgt = 0;
        out("MADT: enabled local APICs:");
        for (u32 e = 44; e + 8 <= hdr[1] && buf[e + 1] >= 2; e += buf[e + 1])
            if (buf[e] == 0 && (*(const u32 *) (buf + e + 4) & 1)) {
                out(" %u", buf[e + 3]);
                if (buf[e + 3] != bsp && g_ntgt < 16)
                    g_tgt[g_ntgt++] = buf[e + 3];
            }
        out("\n");
        return;
    }
    out("MADT: none, IPIs by broadcast\n");
}

static void ipi_one(u32 hi, u32 lo) {
    g_apic[0x310 / 4] = hi;
    g_apic[0x300 / 4] = lo;
    for (int i = 0; i < 100000 && (g_apic[0x300 / 4] & 0x1000u); i++)
        cpu_pause();
}

static void ipi(u32 lo) {
    if (g_ntgt < 0) {
        ipi_one(0, 0xc0000u | lo); /* all excluding self */
        return;
    }
    for (int i = 0; i < g_ntgt; i++)
        ipi_one(g_tgt[i] << 24, lo);
}

static void set_desc(u8 *d, u32 base, u8 access) {
    d[0] = 0xff;
    d[1] = 0xff;
    d[2] = (u8) base;
    d[3] = (u8) (base >> 8);
    d[4] = (u8) (base >> 16);
    d[5] = access;
    d[6] = 0xcf; /* 4 KB granularity, 32 bit, limit 19:16 = F */
    d[7] = (u8) (base >> 24);
}

/* Guard between trampoline and ap_entry, in DOS memory (identity-mapped
 * either way): compares a heap word and the first 8 code bytes of ap_entry
 * as the AP sees them with what the main core wrote. Only if they match
 * does it jump on (tail call: return address and idx stay on the stack);
 * otherwise it counts the AP as bad and halts. Without this, an AP with a
 * wrong address space would run into random memory. */
static u8 *emit32(u8 *p, u32 v) {
    memcpy(p, &v, 4);
    return p + 4;
}

static void build_guard(u8 *tr, u32 page) {
    u32 d = page + TR_GDATA;
    u8 *p = tr + TR_GUARD, *jne[3];
    *p++ = 0x8b, *p++ = 0x35, p = emit32(p, d + 0);  /* mov esi, [heap addr] */
    *p++ = 0x8b, *p++ = 0x06;                        /* mov eax, [esi] */
    *p++ = 0x3b, *p++ = 0x05, p = emit32(p, d + 4);  /* cmp eax, [magic] */
    *p++ = 0x75, jne[0] = p++;                       /* jne bad */
    *p++ = 0x8b, *p++ = 0x35, p = emit32(p, d + 8);  /* mov esi, [code addr] */
    *p++ = 0x8b, *p++ = 0x06;                        /* mov eax, [esi] */
    *p++ = 0x3b, *p++ = 0x05, p = emit32(p, d + 12); /* cmp eax, [code0] */
    *p++ = 0x75, jne[1] = p++;
    *p++ = 0x8b, *p++ = 0x46, *p++ = 0x04;           /* mov eax, [esi+4] */
    *p++ = 0x3b, *p++ = 0x05, p = emit32(p, d + 16); /* cmp eax, [code1] */
    *p++ = 0x75, jne[2] = p++;
    *p++ = 0xff, *p++ = 0x25, p = emit32(p, d + 20); /* jmp [entry] */
    for (int i = 0; i < 3; i++)
        *jne[i] = (u8) (p - (jne[i] + 1));
    *p++ = 0xf0, *p++ = 0xff, *p++ = 0x05, p = emit32(p, d + 24); /* lock inc [bad] */
    *p++ = 0xfa, *p++ = 0xf4, *p++ = 0xeb, *p++ = 0xfc;           /* cli; hlt; jmp cli */
    u32 *g = (u32 *) (tr + TR_GDATA);
    g[0]   = (u32) g_heap_buf;
    g[1]   = g_heap_buf[0];
    g[2]   = (u32) ap_entry;
    g[3]   = ((const u32 *) (void *) ap_entry)[0];
    g[4]   = ((const u32 *) (void *) ap_entry)[1];
    g[5]   = (u32) ap_entry;
    g[6]   = 0;
}

static void start_aps(u32 cr0, int paging) {
    unsigned sel, seg = dos_alloc(0x200, &sel);
    if (!seg) {
        out("AP: no DOS memory\n");
        return;
    }
    u32 page = ((u32) seg * 16 + 0xfff) & ~0xfffu;
    u32 phys, pde, pte;
    if (paging && (!translate(page, &phys, &pde, &pte) || phys != page)) {
        out("AP: trampoline page %08x not identity-mapped (phys %08x) -> no AP start\n", page, phys);
        dos_free(sel);
        return;
    }
    u8 *stacks = (u8 *) malloc(AP_STACK * AP_MAX + 16);
    u8 *tr     = (u8 *) page;
    memset(tr, 0, 0x1000);
    memcpy(tr, ap_tramp_bin, sizeof ap_tramp_bin);
    u8 *gdt = tr + TR_GDT;
    memset(gdt, 0, 8);
    set_desc(gdt + 8, 0, 0x9a);  /* ring-0 code */
    set_desc(gdt + 16, 0, 0x92); /* ring-0 data */
    tr_params *pr = (tr_params *) (tr + P_BASE);
    pr->gdt_limit = 23;
    pr->gdt_base  = page + TR_GDT;
    pr->idt_limit = 32 * 8 - 1;
    pr->idt_base  = page + TR_IDT;
    pr->jmp_off   = page + TR_PM32;
    pr->jmp_sel   = 0x08;
    pr->dsel      = 0x10;
    pr->cr0       = cr0;
    pr->cr4       = paging ? g_cr4 : 0;
    pr->cr3       = paging ? g_cr3 : 0;
    pr->count     = 0;
    pr->max       = AP_MAX;
    pr->stack     = ((u32) stacks + 15) & ~15u;
    pr->stsize    = AP_STACK;
    pr->entry     = page + TR_GUARD;
    build_guard(tr, page);
    {   /* the trampoline's own guard: heap word and the code at P_ENTRY */
        u32 *tg = (u32 *) (tr + TR_TGUARD);
        tg[0]   = (u32) g_heap_buf;
        tg[1]   = g_heap_buf[0];
        tg[2]   = ((u32 *) (tr + TR_GUARD))[0];
        tg[3]   = ((u32 *) (tr + TR_GUARD))[1];
        tg[4]   = 0;
    }
    for (int v = 0; v < 32; v++) {
        u8 *g = tr + TR_IDT + 8 * v;
        u32 o = page + TR_STUBS + 8 * v;
        g[0]  = (u8) o;
        g[1]  = (u8) (o >> 8);
        g[2]  = 0x08;
        g[3]  = 0;
        g[4]  = 0;
        g[5]  = 0x8e;
        g[6]  = (u8) (o >> 16);
        g[7]  = (u8) (o >> 24);
    }
    out("AP: trampoline %08x, CR0 %08x CR3 %08x CR4 %08x, stacks %08x, entry %08x\n", page, pr->cr0, pr->cr3,
        pr->cr4, pr->stack, pr->entry);
    ipi(0x4500); /* INIT, assert */
    wait_us(10000);
    for (int k = 0; k < 2; k++) {
        ipi(0x4600 | (page >> 12)); /* SIPI */
        wait_us(200);
    }
    wait_us(100000);
    u32 n = pr->count;
    out("AP: %u started, stopped by the guards: %u (trampoline) %u (probe), exceptions %u", n,
        ((u32 *) (tr + TR_TGUARD))[4], ((u32 *) (tr + TR_GDATA))[6], pr->exc_flag);
    if (pr->exc_flag)
        out(" (vector %u, %08x %08x %08x)", pr->exc_vec, pr->exc_w[0], pr->exc_w[1], pr->exc_w[2]);
    out("\n");
    u32 want = sum_of(g_heap_buf, 1024);
    for (u32 i = 0; i < n && i < AP_MAX; i++)
        out("  AP %u: APIC %u, done %u, sum %s, heap write %s\n", i, g_ap_apic[i], g_ap_done[i],
            g_ap_sum[i] == want ? "OK" : "WRONG", g_heap_buf[1024 + i] == (0xa0000000u | i) ? "OK" : "missing");
    ipi(0x4500); /* back to wait-for-SIPI */
    wait_us(1000);
    out("AP: INIT sent, back to wait-for-SIPI\n");
    dos_free(sel);
    free(stacks);
}

int main(int argc, char **argv) {
    unsigned r[4], cs = get_cs(), ds = get_ds();
    int      want_ap = argc > 1 && (argv[1][0] == 'A' || argv[1][0] == 'a');
    g_ds             = (u16) ds;
    out("=== R3PROBE %s ===\n", want_ap ? "AP" : "");
    {
        union REGS v;
        memset(&v, 0, sizeof v);
        v.w.ax = 0x0400;
        int386(0x31, &v, &v);
        out("DPMI %u.%02u, flags %04x, CPU %u\n", v.h.ah, v.h.al, v.w.bx, v.h.cl);
    }
    {
        union REGS v;
        memset(&v, 0, sizeof v);
        v.w.ax = 0x1600;
        int386(0x2f, &v, &v);
        out("INT 2Fh 1600h: AX %04x", v.w.ax);
        memset(&v, 0, sizeof v);
        v.w.ax = 0x160a;
        int386(0x2f, &v, &v);
        out(", 160Ah: AX %04x BX %04x CX %04x", v.w.ax, v.w.bx, v.w.cx);
        memset(&v, 0, sizeof v);
        v.w.ax = 0x1683;
        int386(0x2f, &v, &v);
        out(", 1683h: AX %04x BX %04x\n", v.w.ax, v.w.bx);
    }
    out("CPL %u, CS %04x base %08x limit %08x, DS %04x base %08x limit %08x\n", cs & 3, cs, sel_base(cs), lsl(cs),
        ds, sel_base(ds), lsl(ds));
    cpuid4(1, 0, r);
    out("CPUID 1: EBX %08x ECX %08x EDX %08x (APIC %u x2APIC %u PSE %u PAE %u PGE %u FXSR %u, APIC ID %u)\n", r[1],
        r[2], r[3], (r[3] >> 9) & 1, (r[2] >> 21) & 1, (r[3] >> 3) & 1, (r[3] >> 6) & 1, (r[3] >> 13) & 1,
        (r[3] >> 24) & 1, r[1] >> 24);
    unsigned bsp_id = r[1] >> 24, cpu_pse = (r[3] >> 3) & 1;

    g_heap_buf = (u32 *) malloc(64 * 1024);
    for (int i = 0; i < 1024; i++)
        g_heap_buf[i] = (u32) i * 2654435761u + 0x1234567u;
    int ok = dpmi_get_exc(6, g_old6) & dpmi_get_exc(13, g_old13) & dpmi_get_exc(14, g_old14);
    ok &= dpmi_set_exc(6, exc6) & dpmi_set_exc(13, exc13) & dpmi_set_exc(14, exc14);
    out("DPMI 0202h/0203h exception handlers: %s\n", ok ? "installed" : "FAILED");
    if (!ok)
        return 1;

    u32 v, lo = 0, hi = 0, f;
    f = TRY(3, v = rd_cr0());
    out("mov eax, cr0: %s %08x\n", f ? "fault" : "value", v);
    u32 cr0 = f ? 0 : v;
    f = TRY(3, v = rd_cr3());
    out("mov eax, cr3: %s %08x\n", f ? "fault" : "value", v);
    int cr3_ok = !f && v;
    g_cr3      = v;
    f          = TRY(3, v = rd_cr4());
    out("mov eax, cr4: %s %08x\n", f ? "fault" : "value", v);
    g_cr4_ok = !f;
    g_cr4    = f ? 0 : v;
    if (g_cr4_ok) {
        f = TRY(3, wr_cr4(g_cr4));
        out("mov cr4, eax (same value): %s\n", f ? "fault" : "accepted");
        f = TRY(3, v = rd_cr4());
        out("  CR4 afterwards: %08x\n", v);
    }
    f = TRY(3, sse_op());
    out("xorps (SSE usable for the JIT?): %s\n", f ? (f == 6 ? "#UD, OSFXSR off" : "fault") : "OK");
    f = TRY(2, rdmsr2(0x1b, &lo, &hi));
    out("rdmsr 1Bh (APIC base): %s %08x:%08x\n", f ? "fault" : "value", hi, lo);
    {
        u8 raw[6];
        sgdt(raw);
        u32 gb = raw[2] | (raw[3] << 8) | (raw[4] << 16) | ((u32) raw[5] << 24);
        f      = TRY(2, v = rd_mem((void *) (gb + 8)));
        out("GDT %08x limit %04x, read from ring 3: %s\n", gb, raw[0] | (raw[1] << 8),
            f ? (f == 14 ? "page fault" : "fault") : "readable");
    }

    if (!cr3_ok) {
        out("no CR3 -> no page walk, no AP start\n");
    } else if (g_cr4 & 0x20) {
        out("CR4.PAE set -> not handled here\n");
        cr3_ok = 0;
    } else if (!(g_pd = (u32 *) map_phys(g_cr3 & 0xfffff000u, 4096))) {
        out("page directory: DPMI 0800h refused %08x\n", g_cr3 & 0xfffff000u);
        cr3_ok = 0;
    } else {
        int big = 0, glob = 0, used = 0;
        for (int i = 0; i < 1024; i++)
            if (g_pd[i] & 1) {
                used++;
                big += (g_pd[i] & 0x80) != 0;
                glob += (g_pd[i] & 0x100) != 0;
            }
        out("page directory %08x mapped at %08x: %d entries, %d 4-MB pages, %d global\n", g_cr3 & 0xfffff000u,
            (u32) g_pd, used, big, glob);
        if (big && !g_cr4_ok) {
            g_cr4 |= cpu_pse ? 0x10 : 0;
            out("  4-MB pages but CR4 unknown: AP gets CR4.PSE\n");
        }
        static volatile u32 stat_var;
        volatile u32        stack_var;
        unsigned dsel, dseg = dos_alloc(64, &dsel);
        out("translation (write via alias of the physical page, read back):\n");
        check_lin("static", &stat_var, 1);
        check_lin("stack", &stack_var, 1);
        check_lin("heap", g_heap_buf, 1);
        check_lin("heap+60K", g_heap_buf + 15 * 1024, 1);
        check_lin("code", (volatile u32 *) (void *) sum_of, 0);
        if (dseg)
            check_lin("DOS memory", (volatile u32 *) (dseg * 16), 1);
        {
            u32 phys, pde, pte;
            int ok1 = 1;
            for (u32 a = 0; a < 0xa0000; a += 4096)
                if (!translate(a, &phys, &pde, &pte) || phys != a) {
                    out("  first 640 KB: %08x -> %08x (not identity)\n", a, phys);
                    ok1 = 0;
                    break;
                }
            if (ok1)
                out("  first 640 KB: identity-mapped\n");
        }
        if (dseg)
            dos_free(dsel);
    }

    /* local APIC */
    u32 apic_phys = (!lo && !hi) ? 0xfee00000u : (lo & 0xfffff000u);
    g_apic        = (volatile u32 *) map_phys(apic_phys, 4096);
    if (!g_apic) {
        out("local APIC %08x: DPMI 0800h refused\n", apic_phys);
    } else {
        u32 id = g_apic[0x20 / 4], ver = g_apic[0x30 / 4], svr = g_apic[0xf0 / 4];
        out("local APIC %08x at %08x: ID %08x (%u, CPUID %u) version %08x SVR %08x\n", apic_phys, (u32) g_apic, id,
            id >> 24, bsp_id, ver, svr);
        if ((id >> 24) != bsp_id || (ver & 0xff) < 0x10 || (ver & 0xff) > 0x1f)
            g_apic = NULL, out("  -> not a usable xAPIC (x2APIC mode or not mapped)\n");
    }
    /* a host without paging maps physical memory 1:1 */
    int nopg = !cr3_ok && g_apic == (volatile u32 *) apic_phys;
    if (nopg)
        out("CR3 unknown, DPMI 0800h returns linear = physical: host probably without paging\n");

    if (want_ap) {
        if (!(cr3_ok || nopg) || !g_apic) {
            out("AP: skipped (needs CR3 or no paging, and xAPIC)\n");
        } else {
            calibrate();
            out("TSC %u kHz\n", g_tsc_per_ms);
            if (!cr0) {
                cr0 = cr3_ok ? 0x80000033u : 0x33u;
                out("AP: CR0 unknown, using %08x\n", cr0);
            }
            plan_targets(bsp_id);
            start_aps(cr0, cr3_ok);
        }
    }
    dpmi_restore_exc(6, g_old6);
    dpmi_restore_exc(13, g_old13);
    dpmi_restore_exc(14, g_old14);
    out("done\n");
    return 0;
}
