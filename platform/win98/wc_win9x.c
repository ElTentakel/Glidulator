/* Write-combining for the display adapter's framebuffer under Windows 9x.
 *
 * The DLL outputs through GDI; the display driver copies every frame into
 * video memory. Generic drivers (VBEMP) leave the framebuffer uncached
 * (UC) under Windows 9x, about 48 MB/s on current hardware; WC makes that
 * copy several times faster. Marking the physical framebuffer WC with an
 * MTRR needs ring 0, which the DLL reaches without a VxD:
 *
 * - Under Windows 9x the GDT is writable from ring 3 (SGDT gives its
 *   linear address). A free entry is taken atomically (cmpxchg8b against
 *   0), set to a call gate (DPL 3) to wc9x_r0_entry through the system's
 *   flat ring-0 code selector, reached with lcall and cleared again.
 *   Nothing stays behind. Validated in a Windows 98 VM and on a
 *   Core i7-4770T.
 * - Ring 0 finds the framebuffer by PCI configuration (port CF8h/CFCh):
 *   the prefetchable memory BAR of the first display controller with
 *   memory decode on. Its size is measured with the decode switched off
 *   meanwhile (otherwise an Intel HD 4600 keeps a garbled desktop).
 * - mtrr_plan.c computes the new MTRR layout in ring 3; ring 0 writes it
 *   with Intel's procedure (interrupts off, cache off, WBINVD, MTRRs off,
 *   write, on). wc_win9x_off() writes the saved layout back, but only if
 *   the registers still hold ours.
 *
 * The ring-0 code runs with interrupts off and must not page-fault, so its
 * code (section .text$wcr0*, sorted together by the linker) and its data
 * are touched page by page right before each call. Never on Windows NT (no
 * access to the GDT) or Wine. Off with GLIDULATOR_WC=0. */

#define _WIN32_WINNT 0x0400
#include <windows.h>

#include "wc_win9x.h"

#include "glidulator/platform.h"
#include "../common/mtrr_plan.h"

#include <cpuid.h>
#include <setjmp.h>
#include <string.h>

#define MSR_MTRRCAP 0xfe
#define MSR_DEFTYPE 0x2ff
#define MSR_VAR0    0x200
#define CR0_CD      0x40000000u
#define CR0_NW      0x20000000u

/* Shared between ring 3 and ring 0 (both touched before each ring-0 call). */
static struct {
    unsigned          cr4, deftype_lo, deftype_hi, mtrrcap_lo;
    mtrr_state        cur;        /* MTRRs read in ring 0 */
    int               pci_found;
    unsigned          pci_bdf, pci_id, pci_bar;
    uint32_t          lfb_base, lfb_size;
    const mtrr_state *target;     /* layout to write (NULL = only read) */
    int               do_pci;     /* run the PCI/LFB search this call */
} g_r0;

static mtrr_state g_saved, g_plan;
static int        g_on;

#ifdef GLIDULATOR_TRACE
static void wclog(const char *s);
static void wclog2(const char *s, unsigned a, unsigned b);
#    define WCLOG(s) wclog(s)
#else
#    define WCLOG(s)          ((void) 0)
#    define wclog2(s, a, b)   ((void) 0)
#endif

/* ---- ring 0 ------------------------------------------------------------ */

#define R0 __attribute__((section(".text$wcr0b"), noinline, used))

void (*volatile wc9x_r0_fn)(void);
extern char     wc9x_r0_begin[], wc9x_r0_end[];

/* Gate target: interrupts off, all registers kept, calls wc9x_r0_fn. The
 * markers wc9x_r0_begin/_end bracket the whole ring-0 region. */
void wc9x_r0_entry(void);
__asm__(".section .text$wcr0a,\"x\"\n"
        "_wc9x_r0_begin:\n"
        ".section .text$wcr0b,\"x\"\n"
        ".globl _wc9x_r0_entry\n"
        "_wc9x_r0_entry:\n"
        "  pushfl\n"
        "  cli\n"
        "  pushal\n"
        "  cld\n"
        "  call *_wc9x_r0_fn\n"
        "  popal\n"
        "  popfl\n"
        "  lret\n"
        ".section .text$wcr0z,\"x\"\n"
        "_wc9x_r0_end:\n"
        ".text\n");

static inline uint64_t rdmsr64(unsigned msr) {
    unsigned lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t) hi << 32) | lo;
}
static inline void wrmsr64(unsigned msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((unsigned) v), "d"((unsigned) (v >> 32)));
}
static inline unsigned get_cr0(void) {
    unsigned v;
    __asm__ volatile("movl %%cr0, %0" : "=r"(v));
    return v;
}
static inline void set_cr0(unsigned v) {
    __asm__ volatile("movl %0, %%cr0" : : "r"(v) : "memory");
}
static inline unsigned get_cr4(void) {
    unsigned v;
    __asm__ volatile("movl %%cr4, %0" : "=r"(v));
    return v;
}
static inline void flush_tlb(void) {
    unsigned v;
    __asm__ volatile("movl %%cr3, %0\n\tmovl %0, %%cr3" : "=r"(v) : : "memory");
}
static inline unsigned inpd(unsigned port) {
    unsigned v;
    __asm__ volatile("inl %%dx, %%eax" : "=a"(v) : "d"((unsigned short) port));
    return v;
}
static inline void outpd(unsigned port, unsigned v) {
    __asm__ volatile("outl %%eax, %%dx" : : "a"(v), "d"((unsigned short) port));
}
static inline unsigned cfg_rd(unsigned bdf, unsigned off) {
    outpd(0xcf8, 0x80000000u | (bdf << 8) | (off & 0xfc));
    return inpd(0xcfc);
}
static inline void cfg_wr(unsigned bdf, unsigned off, unsigned v) {
    outpd(0xcf8, 0x80000000u | (bdf << 8) | (off & 0xfc));
    outpd(0xcfc, v);
}

/* Size of a memory BAR: all-ones, read back, restore -- with the memory
 * decode off meanwhile (command register bit 1). */
R0 static uint32_t r0_bar_size(unsigned bdf, unsigned off, unsigned orig) {
    unsigned cmd = cfg_rd(bdf, 0x04) & 0xffffu;
    cfg_wr(bdf, 0x04, cmd & ~2u); /* also 0 to the write-1-to-clear status bits */
    cfg_wr(bdf, off, 0xffffffffu);
    unsigned probe = cfg_rd(bdf, off);
    cfg_wr(bdf, off, orig);
    cfg_wr(bdf, 0x04, cmd);
    unsigned mask = probe & 0xfffffff0u;
    return mask ? ~mask + 1u : 0;
}

/* First display controller (class 03h) with memory decode on and a
 * prefetchable 32-bit memory BAR below 4 GB. */
R0 static void r0_find_lfb(void) {
    for (unsigned bus = 0; bus < 256; bus++)
        for (unsigned dev = 0; dev < 32; dev++) {
            unsigned bdf0 = (bus << 8) | (dev << 3);
            if ((cfg_rd(bdf0, 0) & 0xffff) == 0xffff)
                continue;
            unsigned nf = (cfg_rd(bdf0, 0x0c) & 0x800000u) ? 8 : 1;
            for (unsigned fn = 0; fn < nf; fn++) {
                unsigned bdf = bdf0 | fn, id = cfg_rd(bdf, 0);
                if ((id & 0xffff) == 0xffff || (cfg_rd(bdf, 0x08) >> 24) != 0x03 || !(cfg_rd(bdf, 0x04) & 2))
                    continue;
                for (unsigned i = 0; i < 6; i++) {
                    unsigned off = 0x10 + i * 4, bar = cfg_rd(bdf, off);
                    int      is64 = (bar & 7) == 4;
                    if ((bar & 9) == 8 && (bar & 0xfffffff0u) && !(is64 && i < 5 && cfg_rd(bdf, off + 4))) {
                        g_r0.pci_found = 1;
                        g_r0.pci_bdf   = bdf;
                        g_r0.pci_id    = id;
                        g_r0.pci_bar   = off;
                        g_r0.lfb_base  = bar & 0xfffffff0u;
                        g_r0.lfb_size  = r0_bar_size(bdf, off, bar);
                        return;
                    }
                    if (is64)
                        i++; /* skip the high dword */
                }
            }
        }
}

/* Read MTRRs (and, on request, run the PCI/LFB search). */
R0 static void r0_read(void) {
    g_r0.cr4        = get_cr4();
    g_r0.mtrrcap_lo = (unsigned) rdmsr64(MSR_MTRRCAP);
    uint64_t def    = rdmsr64(MSR_DEFTYPE);
    g_r0.deftype_lo = (unsigned) def;
    g_r0.deftype_hi = (unsigned) (def >> 32);
    int n           = g_r0.mtrrcap_lo & 0xff;
    if (n > MTRR_MAX_VAR)
        n = MTRR_MAX_VAR;
    g_r0.cur.count = n;
    for (int i = 0; i < n; i++) {
        g_r0.cur.var[i].base = rdmsr64(MSR_VAR0 + 2 * i);
        g_r0.cur.var[i].mask = rdmsr64(MSR_VAR0 + 2 * i + 1);
    }
    if (g_r0.do_pci)
        r0_find_lfb();
}

/* Write g_r0.target with Intel's MP procedure (SDM vol. 3, for one core). */
R0 static void r0_apply(void) {
    const mtrr_state *s   = g_r0.target;
    unsigned          cr0 = get_cr0();
    set_cr0((cr0 | CR0_CD) & ~CR0_NW);
    __asm__ volatile("wbinvd" ::: "memory");
    flush_tlb();
    wrmsr64(MSR_DEFTYPE, (uint64_t) g_r0.deftype_lo & ~0x800ull); /* MTRRs off */
    for (int i = 0; i < s->count; i++) {
        wrmsr64(MSR_VAR0 + 2 * i, s->var[i].base);
        wrmsr64(MSR_VAR0 + 2 * i + 1, s->var[i].mask);
    }
    __asm__ volatile("wbinvd" ::: "memory");
    flush_tlb();
    wrmsr64(MSR_DEFTYPE, ((uint64_t) g_r0.deftype_hi << 32) | g_r0.deftype_lo); /* back on */
    set_cr0(cr0);
}

/* ---- ring 3 ------------------------------------------------------------ */

/* Read one byte from every page of a range so it is resident (no page
 * fault with interrupts off in ring 0). */
static void touch(const void *p, size_t n) {
    volatile const unsigned char *b = p;
    static volatile unsigned char sink;
    for (size_t i = 0; i < n; i += 0x1000)
        sink = b[i];
    sink = b[n - 1];
    (void) sink;
}

static void touch_r0(void) {
    touch(wc9x_r0_begin, (size_t) (wc9x_r0_end - wc9x_r0_begin));
    touch(&g_r0, sizeof g_r0);
    touch(&g_saved, sizeof g_saved);
    touch(&g_plan, sizeof g_plan);
    touch((void *) &wc9x_r0_fn, sizeof wc9x_r0_fn);
}

/* A fault on the far call would crash the program; catch it with a classic
 * FS:[0] SEH frame (no vectored handlers on Win9x) and report failure. */
static jmp_buf g_jb;
typedef struct seh_frame {
    struct seh_frame *prev;
    EXCEPTION_DISPOSITION(__cdecl *handler)(EXCEPTION_RECORD *, void *, CONTEXT *, void *);
} seh_frame;

static EXCEPTION_DISPOSITION __cdecl cg_seh(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp) {
    (void) rec;
    (void) frame;
    (void) ctx;
    (void) disp;
    longjmp(g_jb, 1);
    return ExceptionContinueExecution; /* not reached */
}

static unsigned *g_gdt;
static int       g_nent;
static int       g_r0sel; /* flat ring-0 code selector (byte offset) */

/* GDT base/limit and a flat ring-0 code selector; 0 if this is not a
 * writable-GDT Windows 9x. */
static int gdt_setup(void) {
    if (!(GetVersion() & 0x80000000u)) /* NT family: GDT not reachable */
        return 0;
    unsigned char gdtr[6];
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    unsigned limit = gdtr[0] | (gdtr[1] << 8);
    unsigned base  = gdtr[2] | (gdtr[3] << 8) | (gdtr[4] << 16) | ((unsigned) gdtr[5] << 24);
    g_nent         = (int) ((limit + 1) / 8);
    if (IsBadReadPtr((void *) base, limit + 1) || IsBadWritePtr((void *) base, limit + 1))
        return 0;
    g_gdt   = (unsigned *) base;
    g_r0sel = 0;
    for (int i = 1; i < g_nent; i++) {
        unsigned lo = g_gdt[2 * i], hi = g_gdt[2 * i + 1];
        unsigned dbase = (lo >> 16) | ((hi & 0xff) << 16) | (hi & 0xff000000u);
        unsigned limh  = (lo & 0xffff) | (hi & 0xf0000);
        if (hi & 0x800000)
            limh = (limh << 12) | 0xfff;
        int type = (hi >> 8) & 15, s = (hi >> 12) & 1, dpl = (hi >> 13) & 3, p = (hi >> 15) & 1, db = (hi >> 22) & 1;
        if (p && s && (type & 8) && dpl == 0 && db && dbase == 0 && limh >= 0xfffff000u) {
            g_r0sel = i * 8;
            break;
        }
    }
    return g_r0sel != 0;
}

static int cas8(volatile unsigned *p, unsigned lo, unsigned hi) {
    unsigned char ok;
    unsigned      elo = 0, ehi = 0;
    __asm__ volatile("lock cmpxchg8b %1\n\tsetz %0"
                     : "=q"(ok), "+m"(*(volatile unsigned long long *) p), "+a"(elo), "+d"(ehi)
                     : "b"(lo), "c"(hi)
                     : "memory");
    return ok;
}

/* Install a temporary call gate on a free GDT entry, lcall it, clear it.
 * 0 on success, -1 on no free entry or a fault in ring 0. */
static int r0_call(void (*fn)(void)) {
    volatile int slot = -1; /* used after setjmp: keep across a longjmp */
    for (int i = g_nent - 1; i >= 8; i--) {
        if (g_gdt[2 * i] || g_gdt[2 * i + 1])
            continue;
        unsigned addr    = (unsigned) &wc9x_r0_entry;
        unsigned gate_lo = (addr & 0xffff) | ((unsigned) g_r0sel << 16);
        unsigned gate_hi = (addr & 0xffff0000u) | 0xec00u; /* P=1 DPL=3 type=C (386 call gate) */
        if (cas8(&g_gdt[2 * i], gate_lo, gate_hi)) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    wc9x_r0_fn = fn;
    touch_r0();
    struct {
        unsigned       off;
        unsigned short sel;
    } far_ptr = { 0, (unsigned short) (slot * 8) };
    int       crashed = 0;
    seh_frame frame, *prev;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(prev));
    frame.prev    = prev;
    frame.handler = cg_seh;
    if (setjmp(g_jb)) {
        crashed = 1;
    } else {
        __asm__ volatile("movl %0, %%fs:0" : : "r"(&frame) : "memory");
        __asm__ volatile("lcall *%0" : : "m"(far_ptr) : "memory", "cc");
    }
    __asm__ volatile("movl %0, %%fs:0" : : "r"(prev) : "memory");

    g_gdt[2 * slot]     = 0; /* nothing stays behind */
    g_gdt[2 * slot + 1] = 0;
    return crashed ? -1 : 0;
}

int wc_win9x_on(uint32_t need) {
    char cfg[8];
    unsigned r[4];
    if (g_on)
        return 1;
    if (platform_get_config("GLIDULATOR_WC", cfg, sizeof cfg) && cfg[0] == '0') {
        WCLOG("off (GLIDULATOR_WC=0)");
        return -1;
    }
    if (!__get_cpuid(1, &r[0], &r[1], &r[2], &r[3]) || !(r[3] & (1u << 12))) {
        WCLOG("off (CPU without MTRRs)");
        return -1;
    }
    if (!gdt_setup()) {
        WCLOG("off (no writable GDT -- not Windows 9x?)");
        return -1;
    }

    /* Ring 0: read the MTRRs and find the framebuffer. */
    memset(&g_r0, 0, sizeof g_r0);
    g_r0.do_pci = 1;
    if (r0_call(r0_read) < 0) {
        WCLOG("off (ring 0 not reached)");
        return -1;
    }
    if (!(g_r0.mtrrcap_lo & 0x400) || (g_r0.mtrrcap_lo & 0xff) == 0) {
        WCLOG("off (CPU without WC MTRR type)");
        return -1;
    }
    if (!(g_r0.deftype_lo & 0x800)) {
        WCLOG("off (MTRRs disabled)");
        return -1;
    }
    if (!g_r0.pci_found || !g_r0.lfb_base || !g_r0.lfb_size) {
        WCLOG("off (no framebuffer found)");
        return -1;
    }
    wclog2("display", g_r0.pci_id, g_r0.pci_bdf);
    wclog2("LFB", g_r0.lfb_base, g_r0.lfb_size / 1024);

    /* Plan in ring 3 (portable code, tested). */
    g_saved           = g_r0.cur;
    g_saved.def_type  = (int) (g_r0.deftype_lo & 0xff);
    g_saved.phys_bits = 36;
    if (__get_cpuid(0x80000000u, &r[0], &r[1], &r[2], &r[3]) && r[0] >= 0x80000008u) {
        __get_cpuid(0x80000008u, &r[0], &r[1], &r[2], &r[3]);
        if ((r[0] & 0xff) >= 32)
            g_saved.phys_bits = (int) (r[0] & 0xff);
    }
    uint64_t size = mtrr_wc_size(g_r0.lfb_base, need, g_r0.lfb_size);
    if (!size) {
        WCLOG("off (framebuffer not aligned)");
        return -1;
    }
    int used = 0, rc = mtrr_plan_wc(&g_saved, g_r0.lfb_base, size, &g_plan, &used);
    if (rc == MTRR_PLAN_ALREADY) {
        WCLOG("already on (real driver sets WC itself)");
        return 0;
    }
    if (rc != MTRR_PLAN_DONE) {
        WCLOG(rc == MTRR_PLAN_NOROOM ? "off (too few free MTRRs)" : "off (range overlaps non-UC)");
        return -1;
    }

    /* Ring 0: write the plan. */
    g_r0.target = &g_plan;
    g_r0.do_pci = 0;
    if (r0_call(r0_apply) < 0) {
        WCLOG("off (ring 0 not reached for write)");
        return -1;
    }
    g_on = 1;
    wclog2("on, size KB / MTRRs used", (unsigned) (size / 1024), (unsigned) used);
    return 1;
}

void wc_win9x_off(void) {
    if (!g_on)
        return;
    g_on = 0;
    if (!gdt_setup())
        return;
    /* Restore only if the registers still hold our plan (nothing else
     * touched the MTRRs meanwhile). */
    memset(&g_r0, 0, sizeof g_r0);
    if (r0_call(r0_read) < 0)
        return;
    if (memcmp(&g_r0.cur, &g_plan, sizeof(int) + sizeof g_plan.var[0] * g_plan.count) != 0) {
        WCLOG("off skipped (MTRRs changed by someone else)");
        return;
    }
    g_r0.target = &g_saved;
    r0_call(r0_apply);
    WCLOG("off (original MTRRs restored)");
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
static void wclog(const char *s) {
    char  line[160];
    char *p = put_s(line, "Write-Combining: ");
    p       = put_s(p, s);
    *p++    = '\n';
    *p      = 0;
    platform_log(line);
}
static void wclog2(const char *s, unsigned a, unsigned b) {
    char  line[96];
    char *p = put_s(line, "Write-Combining: ");
    p       = put_s(p, s);
    p       = put_s(p, " ");
    p       = put_hex(p, a);
    p       = put_s(p, " ");
    p       = put_hex(p, b);
    *p++    = '\n';
    *p      = 0;
    platform_log(line);
}
#endif
