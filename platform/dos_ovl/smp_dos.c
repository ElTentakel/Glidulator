/* Additional CPU cores for the OVL's render workers (Open Watcom, ring 0).
 *
 * Under DOS/4GW without memory manager the OVL runs in ring 0 without
 * paging: linear addresses are physical. After the BIOS, the other cores
 * (APs) wait for a Startup IPI; the main core starts them itself, as an
 * operating system does (Intel SDM vol. 3, "MP Initialization"): INIT,
 * 10 ms, two SIPIs. Every AP runs through the trampoline ap_tramp.S (in DOS
 * memory below 1 MB), loads DOS/4GW's GDT and the main core's selectors,
 * CR0 and CR4, and ends up in ap_entry(); from there core/ and the JIT run
 * unchanged.
 *
 * APs live only while workers exist: platform_spawn_worker() starts them
 * (with the first worker), the last platform_join_worker() sends INIT
 * again and parks them in a hlt loop of the BIOS ROM (park_in_rom), and
 * the trampoline's DOS memory is freed. Nothing remains resident. The core
 * count (platform_core_count) is determined once by such a start/stop
 * (~70 ms).
 *
 * One AP per PHYSICAL core is counted and used, excluding the main core's
 * own core: Hyper-Threading siblings share execution units, and a worker on
 * the main core's sibling measurably slowed down the frame output. APs
 * without work halt (hlt) 200 ms after the start until INIT, as does every
 * AP whose worker has finished.
 *
 * Ring 3 under a DPMI host (HDPMI32i or VDPMI for SBEMU, which needs the
 * game in ring 3 to trap its ports): the APs do not belong to the host and
 * still start in ring 0, with an own flat GDT in the trampoline page. They
 * need the main core's linear address space:
 *   - paging host (HDPMI): it emulates reading CR0/CR3/CR4 for ring 3,
 *     the APs load its page directory. The trampoline page must be
 *     identity-mapped (checked by walking the tables via DPMI 0800h). Every
 *     AP reloads CR3 before its job; afterwards no TLB flush is needed: the
 *     OVL frees memory the workers use only after stopping them, and new
 *     mappings are seen anyway (not-present entries are never cached).
 *   - 1:1 host (VDPMI): every linear address it maps equals the physical
 *     one (DPMI 0800h returns the physical address itself; CR3 reads as 0),
 *     so the APs run without paging and see the same memory.
 * The local APIC is mapped via DPMI 0800h (xAPIC only). The trampoline's
 * guard stops any AP whose address space does not match. GLIDULATOR_SMP =
 * auto (default: ring 0 as before, ring 3 detects the host) | ring0 (only
 * the ring-0 path) | paging | flat (force one ring-3 path) | off.
 *
 * Disabled (single-core) when: ring 3 without a usable host (Windows DOS
 * box, CR3 unreadable with paging, selectors not flat, x2APIC), paging on
 * in ring 0 (EMM386/VCPI), CS/DS not flat in the GDT, no CPUID/TSC/APIC/
 * MSR, APIC off or above 4 GB, no DOS memory, under tools/leload_demo
 * (Linux), GLIDULATOR_SMP=off or GLIDULATOR_THREADS=1.
 *
 * The APs' MTRRs are not synchronised (wc_dos.c sets write-combining for
 * the LFB on the main core only): workers write only to the framebuffer in
 * RAM; only the main core touches the LFB. */

#include "glidulator/platform.h"

#include "ap_tramp.h"
#include "host_memory.h"
#include "r3_dos.h"

/* counterpart in ap_tramp.S */
#define P_BASE     0x100
#define TR_IDT     0x400 /* 32 gates of 8 bytes, after the code */
#define TR_STUBS   0x200
#define TR_PM32    0x60
#define TR_APREC   0x500 /* 12 words per AP (ap_tramp.S P_APREC) */
#define TR_GUARD   0x680 /* ap_tramp.S G_*: heap addr, magic, code0, code1, bad */
#define TR_GDT     0x6a0 /* ring 3: own GDT (null, code 08h, data 10h) */
#define TR_BYTES   0x6b8
#define AP_MAX     8
#define AP_STACK   0x10000u

#pragma pack(push, 1)
typedef struct {
    uint16_t gdt_limit;
    uint32_t gdt_base;
    uint16_t pad0;
    uint16_t idt_limit;
    uint32_t idt_base;
    uint16_t pad1;
    uint32_t jmp_off;
    uint16_t jmp_sel;
    uint16_t pad2;
    uint16_t dsel;
    uint16_t pad3;
    uint32_t cr0;
    uint32_t cr4;
    uint32_t count;
    uint32_t max;
    uint32_t stack;
    uint32_t stsize;
    uint32_t entry;
    uint32_t exc_flag;
    uint32_t exc_vec;
    uint32_t exc_w[3];
    uint32_t exc_esp;
    uint32_t mc_n;        /* machine check on the AP: number of banks ... */
    uint32_t mc[10][4];   /* ... STATUS lo/hi, ADDR lo/hi each */
    uint32_t cr3;         /* loaded before CR0 (only matters with CR0.PG) */
} tr_params;

typedef struct {
    uint32_t heap, magic, code0, code1, bad;
} tr_guard;
#pragma pack(pop)

/* ---- CPU instructions (as bytes where the inline assembler lacks them) ---- */

static unsigned get_cs(void);
#pragma aux get_cs = "mov ax, cs" "movzx eax, ax" value [eax];
static unsigned get_ds(void);
#pragma aux get_ds = "mov ax, ds" "movzx eax, ax" value [eax];
static unsigned get_cr0(void);
#pragma aux get_cr0 = "mov eax, cr0" value [eax];
static unsigned get_cr4(void);
#pragma aux get_cr4 = 0x0f 0x20 0xe0 value [eax];
static void sgdt(void *p);
#pragma aux sgdt = 0x0f 0x01 0x00 parm [eax]; /* sgdt [eax] */
static unsigned eflags_id(void);
#pragma aux eflags_id = \
    "pushfd" "pop eax" "mov ecx, eax" "xor eax, 200000h" "push eax" "popfd" \
    "pushfd" "pop eax" "push ecx" "popfd" "xor eax, ecx" value [eax] modify [ecx];
static unsigned cpuid1_edx(void);
#pragma aux cpuid1_edx = "mov eax, 1" 0x0f 0xa2 value [edx] modify [eax ebx ecx];
static void rdmsr2(unsigned msr, unsigned *lo, unsigned *hi);
#pragma aux rdmsr2 = 0x0f 0x32 "mov [esi], eax" "mov [edi], edx" parm [ecx] [esi] [edi] modify [eax edx];
static void wrmsr2(unsigned msr, unsigned lo, unsigned hi);
#pragma aux wrmsr2 = 0x0f 0x30 parm [ecx] [eax] [edx];
static void cpuid_full(unsigned leaf, unsigned sub, unsigned *out);
#pragma aux cpuid_full = \
    "push ebx" 0x0f 0xa2 "mov [edi], eax" "mov [edi+4], ebx" "mov [edi+8], ecx" \
    "mov [edi+12], edx" "pop ebx" parm [eax] [ecx] [edi] modify [eax ecx edx];
static void rdtsc2(uint32_t *v);
#pragma aux rdtsc2 = 0x0f 0x31 "mov [ecx], eax" "mov [ecx+4], edx" parm [ecx] modify [eax edx];
static unsigned cas32(volatile int *p, int expect, int val);
#pragma aux cas32 = "lock cmpxchg [edx], ecx" parm [edx] [eax] [ecx] value [eax];
static void park_forever(void);
#pragma aux park_forever = "again:" "cli" "hlt" "jmp short again" aborts;
static void cpu_pause(void);
#pragma aux cpu_pause = 0xf3 0x90;
static void set_cr3(unsigned v);
#pragma aux set_cr3 = 0x0f 0x22 0xd8 parm [eax]; /* mov cr3, eax */
static unsigned flags_cli(void);
#pragma aux flags_cli = "pushfd" "pop eax" "cli" value [eax];
static void flags_restore(unsigned f);
#pragma aux flags_restore = "push eax" "popfd" parm [eax];
/* Windows in enhanced mode (3.1, 9x): INT 2Fh 160Ah returns AX 0 and
 * CX 3. Not 1600h alone: VDPMI answers that like Windows 4.0 (AL 4), but
 * 160Ah with CX 2 (standard mode). Returns CX, or 0 if unsupported. */
static unsigned windows_mode(void);
#pragma aux windows_mode = "mov ax, 160ah" "xor ecx, ecx" "int 2fh" "test ax, ax" "jz short win_ok" "xor ecx, ecx" \
    "win_ok:" "movzx eax, cx" value [eax] modify [ebx ecx edx];
static void fnstcw(uint16_t *p);
#pragma aux fnstcw = 0x9b 0xd9 0x38 parm [eax]; /* fstcw [eax] */
static void fldcw(const uint16_t *p);
#pragma aux fldcw = 0xd9 0x28 parm [eax];
static void stmxcsr(uint32_t *p);
#pragma aux stmxcsr = 0x0f 0xae 0x18 parm [eax];
static void ldmxcsr(const uint32_t *p);
#pragma aux ldmxcsr = 0x0f 0xae 0x10 parm [eax];

/* DPMI 0100h: DOS memory (paragraphs in BX) -> AX segment, DX selector */
static unsigned dos_alloc(unsigned paras, unsigned *sel);
#pragma aux dos_alloc = \
    "push edx"         \
    "mov ebx, eax"     \
    "mov ax, 0100h"    \
    "int 31h"          \
    "pop ebx"          \
    "jc short fail"    \
    "mov [ebx], edx"   \
    "movzx eax, ax"    \
    "jmp short done"   \
    "fail:"            \
    "xor eax, eax"     \
    "done:"            \
    parm [eax] [edx] value [eax] modify [ebx ecx edx];
static void dos_free(unsigned sel);
#pragma aux dos_free = "mov ax, 0101h" "int 31h" parm [edx] modify [eax];

#define CR0_PG       0x80000000u
#define CR4_PSE      0x10u
#define CR4_PAE      0x20u
#define CR4_OSFXSR   0x200u
#define MSR_APICBASE 0x1b
#define MSR_X2_ICR   0x830
#define APIC_EN      0x800u /* MSR 1Bh: global enable */
#define APIC_SVR     0x0f0
#define APIC_LVT0    0x350
#define APIC_LVT1    0x360
#define ICR_LO       0x300
#define ICR_HI       0x310
#define ICR_PENDING  0x1000u
#define ICR_ALL_BUT_SELF 0xc0000u
#define ICR_ASSERT   0x4000u
#define ICR_INIT     0x500u
#define ICR_SIPI     0x600u

/* ---- State ---- */

#define SLOT_WAIT  0
#define SLOT_JOB   1 /* fn/arg valid, AP working */
#define SLOT_PARK  2 /* AP halted (hlt) until INIT */

typedef struct {
    platform_worker_fn fn;
    void              *arg;
    int                state; /* SLOT_*; transitions via lock cmpxchg */
    int                done;  /* worker function returned */
    int                ready; /* apic valid */
    int                lost;  /* died or abandoned: stop waiting */
    unsigned           apic;  /* APIC ID of this AP */
} ap_slot;

struct platform_worker {
    int idx;
};

static int                 g_state; /* 0 unchecked, 1 possible, -1 not */
#define MODE_RING0  1 /* DOS/4GW without host: its GDT, no paging */
#define MODE_PAGING 2 /* ring 3, host's page tables (HDPMI) */
#define MODE_FLAT   3 /* ring 3, host maps 1:1, APs without paging (VDPMI) */
static int                 g_mode;
static uint32_t            g_r3_cr0, g_r3_cr3, g_r3_cr4;
static uint32_t           *g_pd; /* MODE_PAGING: host's page directory (0800h) */
static int                 g_count = -1; /* usable APs (one per physical core) */
static int                 g_order[AP_MAX];   /* worker i -> slot g_order[i] */
static int                 g_norder;          /* entries in g_order */
static volatile uint32_t   g_release[2];      /* TSC at the end of start_aps */
static volatile uint32_t   g_park_ticks;      /* halt after this (0 = not yet) */
static int                 g_running, g_active;
static int                 g_x2apic;
static volatile uint32_t  *g_apic;
static unsigned            g_dos_sel;
static uint8_t            *g_tr;
static uint8_t            *g_stacks;
static uint16_t            g_fpu_cw;
static uint32_t            g_mxcsr;
static int                 g_have_mxcsr;
static volatile ap_slot    g_slot[AP_MAX];
static struct platform_worker g_worker[AP_MAX];

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

static void log_smp(const char *what, int n) {
    char  line[120];
    char *p = put_s(line, "Multi-core: ");
    p       = put_s(p, what);
    if (n >= 0)
        p = put_dec(p, (unsigned) n);
    *p++ = '\n';
    *p   = 0;
    platform_log(line);
}

#    ifdef GLIDULATOR_DEBUG
static void log_exc(const tr_params *pr) {
    char  line[200];
    char *p = put_s(line, "Multi-core: exception ");
    p       = put_dec(p, pr->exc_vec);
    p       = put_s(p, " on an AP, stack ");
    for (int i = 0; i < 3; i++) {
        p    = put_hex(p, pr->exc_w[i]);
        *p++ = ' ';
    }
    *p++ = '\n';
    *p   = 0;
    platform_log(line);
    for (int a = 0; a < AP_MAX; a++) {
        const uint32_t *r = (const uint32_t *) ((const uint8_t *) pr - P_BASE + TR_APREC + 48 * a);
        static const char *const names[8] = { " EDI ", " ESI ", " EBP ", " ESP ", " EBX ", " EDX ", " ECX ", " EAX " };
        if (r[10] != 1)
            continue;
        p = put_s(line, "  AP slot ");
        p = put_dec(p, (unsigned) a);
        p = put_s(p, ": vector ");
        p = put_dec(p, r[0]);
        p = put_s(p, " EIP ");
        p = put_hex(p, r[1]);
        for (int k = 7; k >= 0; k--) {
            p = put_s(p, names[k]);
            p = put_hex(p, r[2 + k]);
        }
        *p++ = '\n';
        *p   = 0;
        platform_log(line);
    }
    for (unsigned b = 0; pr->exc_vec == 18 && b < pr->mc_n && b < 10; b++) {
        if (!(pr->mc[b][1] & 0x80000000u)) /* VAL */
            continue;
        p = put_s(line, "  AP bank ");
        p = put_dec(p, b);
        p = put_s(p, ": STATUS ");
        p = put_hex(p, pr->mc[b][1]);
        p = put_hex(p, pr->mc[b][0]);
        if (pr->mc[b][1] & (1u << 26)) {
            p = put_s(p, " ADDR ");
            p = put_hex(p, pr->mc[b][3]);
            p = put_hex(p, pr->mc[b][2]);
        }
        *p++ = '\n';
        *p   = 0;
        platform_log(line);
    }
}
#    endif /* GLIDULATOR_DEBUG */
#    define LOG_SMP(w, n) log_smp(w, n)
#else
#    define LOG_SMP(w, n) ((void) 0)
#endif

/* Descriptor of a GDT selector: flat (base 0, 4 GB, 32 bit)? */
static int flat_gdt_selector(const uint8_t *gdt, unsigned limit, unsigned sel) {
    if ((sel & 4) || (sel | 7) > limit || sel < 8)
        return 0;
    const uint8_t *d     = gdt + (sel & ~7u);
    unsigned       base  = d[2] | (d[3] << 8) | (d[4] << 16) | ((unsigned) d[7] << 24);
    unsigned       lim   = d[0] | (d[1] << 8) | ((d[6] & 15u) << 16);
    int            big   = (d[6] & 0x40) != 0; /* D/B */
    int            gran  = (d[6] & 0x80) != 0;
    int            pres  = (d[5] & 0x80) != 0;
    return pres && base == 0 && big && gran && lim == 0xfffff;
}

/* GLIDULATOR_SMP */
#define WANT_AUTO   0
#define WANT_RING0  1
#define WANT_PAGING 2
#define WANT_FLAT   3
#define WANT_OFF    4

static int same_word(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != *b)
            return 0;
    return !*a && !*b;
}

static int want_mode(void) {
    char cfg[12];
    if (!platform_get_config("GLIDULATOR_SMP", cfg, sizeof cfg))
        return WANT_AUTO;
    return same_word(cfg, "ring0")    ? WANT_RING0
           : same_word(cfg, "paging") ? WANT_PAGING
           : same_word(cfg, "flat")   ? WANT_FLAT
           : same_word(cfg, "off")    ? WANT_OFF
                                      : WANT_AUTO;
}

#ifdef GLIDULATOR_DEBUG
/* Test of the trampoline's guard (debug OVL): GLIDULATOR_SMP_TEST =
 * flat (ring 3 under a paging host: start the APs without paging, i.e. in
 * a wrong address space) | magic (any mode: wrong expected heap word).
 * Expected: "off (guard ...)", one core, no damage. */
static int smp_test(void) {
    char cfg[8];
    if (!platform_get_config("GLIDULATOR_SMP_TEST", cfg, sizeof cfg))
        return 0;
    return cfg[0] == 'f' ? 1 : cfg[0] == 'm' ? 2 : 0;
}
#endif

/* MODE_PAGING: does linear page lin map to the same physical address? */
static int identity_mapped(uint32_t lin) {
    uint32_t d = g_pd[lin >> 22];
    if (!(d & 1))
        return 0;
    if (d & 0x80)
        return (g_r3_cr4 & CR4_PSE) && (d & 0xffc00000u) == (lin & 0xffc00000u);
    uint32_t *pt = (uint32_t *) r3_map_phys(d & 0xfffff000u, 4096);
    if (!pt)
        return 0;
    uint32_t t = pt[(lin >> 12) & 1023];
    r3_unmap(pt);
    return (t & 1) && (t & 0xfffff000u) == (lin & 0xfffff000u);
}

/* Ring 3: which host, and can the APs share its address space? */
static int check_ring3(int want) {
    uint32_t cr0 = 0, cr3 = 0, cr4 = 0, lo = 0, hi = 0;
    unsigned cs = get_cs(), ds = get_ds(), r[4];
    if (want == WANT_RING0) {
        LOG_SMP("off (not ring 0, GLIDULATOR_SMP=ring0)", -1);
        return 0;
    }
    if (windows_mode() == 3) {
        LOG_SMP("off (ring 3 under Windows)", -1);
        return 0;
    }
    if (r3_sel_base(cs) || r3_sel_base(ds) || r3_sel_limit(cs) != 0xffffffffu || r3_sel_limit(ds) != 0xffffffffu) {
        LOG_SMP("off (ring 3, selectors not flat)", -1);
        return 0;
    }
    if (!r3_guard_begin()) {
        LOG_SMP("off (ring 3, no DPMI exception handling)", -1);
        return 0;
    }
    int ok0 = r3_try_read_cr(0, &cr0), ok3 = r3_try_read_cr(3, &cr3), ok4 = r3_try_read_cr(4, &cr4);
    int okm = r3_try_rdmsr(MSR_APICBASE, &lo, &hi);
    r3_guard_end();
    uint32_t apic = 0xfee00000u;
    if (okm) {
        if (!(lo & 0x800) || !(lo & 0x100) || hi) {
            LOG_SMP("off (APIC disabled, not the BSP, or above 4 GB)", -1);
            return 0;
        }
        if (lo & 0x400) {
            LOG_SMP("off (ring 3, x2APIC)", -1);
            return 0;
        }
        apic = lo & 0xfffff000u;
    }
    volatile uint32_t *a = (volatile uint32_t *) r3_map_phys(apic, 4096);
    cpuid_full(1, 0, r);
    if (!a || (a[0x20 / 4] >> 24) != (r[1] >> 24) || (a[0x30 / 4] & 0xff) < 0x10 || (a[0x30 / 4] & 0xff) > 0x1f) {
        r3_unmap((void *) a);
        LOG_SMP("off (ring 3, no reachable xAPIC)", -1);
        return 0;
    }
    int can_paging = ok0 && ok3 && ok4 && cr3 && (cr0 & CR0_PG) && !(cr4 & CR4_PAE);
    int can_flat   = !(ok3 && cr3) && (uint32_t) a == apic;
#ifdef GLIDULATOR_DEBUG
    if (smp_test() == 1 && can_paging) {
        LOG_SMP("Test: APs without paging under a paging host (GLIDULATOR_SMP_TEST=flat)", -1);
        can_paging = 0;
        can_flat   = 1;
    }
#endif
    int mode       = 0;
    if (want == WANT_PAGING || want == WANT_AUTO)
        mode = can_paging ? MODE_PAGING : 0;
    if (!mode && (want == WANT_FLAT || want == WANT_AUTO))
        mode = can_flat ? MODE_FLAT : 0;
    if (mode == MODE_PAGING && !(g_pd = (uint32_t *) r3_map_phys(cr3 & 0xfffff000u, 4096)))
        mode = 0;
    if (!mode) {
        r3_unmap((void *) a);
        LOG_SMP(want == WANT_PAGING ? "off (ring 3, CR0/CR3/CR4 not readable: GLIDULATOR_SMP=paging impossible)"
                : want == WANT_FLAT ? "off (ring 3, host does not map 1:1: GLIDULATOR_SMP=flat impossible)"
                                    : "off (ring 3, host address space unknown)",
                -1);
        return 0;
    }
    g_mode   = mode;
    g_apic   = a;
    g_x2apic = 0;
    g_r3_cr0 = cr0;
    g_r3_cr3 = cr3;
    g_r3_cr4 = cr4;
    LOG_SMP(mode == MODE_PAGING ? "ring 3, APs on the host's page tables" : "ring 3, APs without paging (host maps 1:1)",
            -1);
    return 1;
}

/* Windows 98 clears the APIC's global enable bit (MSR 1Bh bit 11) and
 * leaves it cleared after "Restart in MS-DOS mode"; CPUID then reports no
 * APIC. In ring 0 the OVL sets the bit again while its APs run and writes
 * the original value back when they stop, so nothing stays changed. Once
 * re-enabled, the APIC is in its reset state: software-disabled with
 * LINT0/LINT1 masked, which would block the 8259's interrupts. It is then
 * programmed like the BIOS does (virtual wire mode). */
static unsigned g_apic_off_lo, g_apic_off_hi; /* original MSR 1Bh value */
static int      g_apic_reenable;              /* the APIC must be switched on for the APs */
static int      g_apic_on;                    /* switched on by us right now */

static int madt_apics(void);

static int apic_enable(void) {
    if (!g_apic_reenable || g_apic_on)
        return 1;
    unsigned f = flags_cli();
    wrmsr2(MSR_APICBASE, g_apic_off_lo | APIC_EN, g_apic_off_hi);
    int ok = (cpuid1_edx() & (1u << 9)) != 0;
    if (ok) {
        volatile uint32_t *a = (volatile uint32_t *) (g_apic_off_lo & 0xfffff000u);
        if (!(a[APIC_SVR / 4] & 0x100)) {
            a[APIC_SVR / 4]  = 0x1ff; /* software enable first: masks cannot be cleared before */
            a[APIC_LVT0 / 4] = 0x700; /* ExtINT: the 8259 through LINT0 */
            a[APIC_LVT1 / 4] = 0x400; /* NMI */
        }
        g_apic_on = 1;
    } else {
        wrmsr2(MSR_APICBASE, g_apic_off_lo, g_apic_off_hi);
    }
    flags_restore(f);
    return ok;
}

static void apic_restore(void) {
    if (!g_apic_on)
        return;
    unsigned f = flags_cli();
    wrmsr2(MSR_APICBASE, g_apic_off_lo, g_apic_off_hi);
    g_apic_on = 0;
    flags_restore(f);
}

static int check(void) {
    unsigned lo, hi;
    if (g_state)
        return g_state > 0;
    g_state = -1;
    if (platform_host_memory_set()) /* tools/leload_demo on Linux */
        return 0;
    int want = want_mode();
    if (want == WANT_OFF) {
        LOG_SMP("off (GLIDULATOR_SMP=off)", -1);
        return 0;
    }
    if (get_cs() & 3) {
        if (!(eflags_id() & 0x200000u)) {
            LOG_SMP("off (no CPUID)", -1);
            return 0;
        }
        unsigned edx = cpuid1_edx();
        if (!(edx & (1u << 9)) || !(edx & (1u << 4))) {
            LOG_SMP("off (no APIC/TSC)", -1);
            return 0;
        }
        if (!check_ring3(want))
            return 0;
    } else {
        if (get_cr0() & CR0_PG) {
            LOG_SMP("off (paging on, e.g. EMM386)", -1);
            return 0;
        }
        if (!(eflags_id() & 0x200000u)) {
            LOG_SMP("off (no CPUID)", -1);
            return 0;
        }
        unsigned edx = cpuid1_edx();
        g_mode       = MODE_RING0; /* phys_copy() for the MADT */
        if (!(edx & (1u << 9)) && (edx & (1u << 5)) && (edx & (1u << 4)) && madt_apics() >= 2) {
            /* No APIC in CPUID, but ACPI lists several: switched off (Windows 98) */
            rdmsr2(MSR_APICBASE, &lo, &hi);
            if (!(lo & APIC_EN) && (lo & 0x100) && !hi && !(lo & 0x400)) {
                g_apic_off_lo   = lo;
                g_apic_off_hi   = hi;
                g_apic_reenable = 1;
                if (apic_enable()) {
                    LOG_SMP("APIC was switched off (e.g. by Windows 98): on while the extra cores run", -1);
                    edx = cpuid1_edx();
                } else {
                    g_apic_reenable = 0;
                }
            }
        }
        g_mode = 0;
        if (!(edx & (1u << 9)) || !(edx & (1u << 5)) || !(edx & (1u << 4))) {
            LOG_SMP("off (no APIC/MSR/TSC)", -1);
            return 0;
        }
        rdmsr2(MSR_APICBASE, &lo, &hi);
        apic_restore(); /* start_aps() switches it on again */
        if (!(lo & APIC_EN) || !(lo & 0x100) || hi) {
            LOG_SMP("off (APIC disabled, not the BSP, or above 4 GB)", -1);
            return 0;
        }
        g_x2apic = (lo & 0x400) != 0;
        g_apic   = (volatile uint32_t *) (lo & 0xfffff000u);
        struct {
            uint16_t limit;
            uint32_t base;
        } gdtr;
        uint8_t raw[6];
        sgdt(raw);
        gdtr.limit = (uint16_t) (raw[0] | (raw[1] << 8));
        gdtr.base  = raw[2] | (raw[3] << 8) | (raw[4] << 16) | ((uint32_t) raw[5] << 24);
        if (!flat_gdt_selector((const uint8_t *) gdtr.base, gdtr.limit, get_cs()) ||
            !flat_gdt_selector((const uint8_t *) gdtr.base, gdtr.limit, get_ds())) {
            LOG_SMP("off (CS/DS not flat in the GDT)", -1);
            return 0;
        }
        g_mode = MODE_RING0;
    }
    if (!platform_time_us()) {
        LOG_SMP("off (no time source)", -1);
        return 0;
    }
    g_state = 1;
    return 1;
}

static void wait_us(uint32_t us) {
    uint64_t t0 = platform_time_us();
    while (platform_time_us() - t0 < us)
        cpu_pause();
}

static void send_ipi_to(unsigned dest, uint32_t lo) {
    if (g_x2apic) {
        wrmsr2(MSR_X2_ICR, lo, dest);
        return;
    }
    g_apic[ICR_HI / 4] = dest << 24;
    g_apic[ICR_LO / 4] = lo;
    for (int i = 0; i < 100000 && (g_apic[ICR_LO / 4] & ICR_PENDING); i++)
        cpu_pause();
}

/* APIC ID of the executing core (x2APIC ID from leaf 0Bh, otherwise the
 * 8-bit ID from leaf 1) and the number of low ID bits that only count the
 * hyperthread within a core. */
static unsigned max_leaf(void) {
    unsigned r[4];
    cpuid_full(0, 0, r);
    return r[0];
}

static unsigned my_apic_id(void) {
    unsigned r[4];
    if (max_leaf() >= 0xb) {
        cpuid_full(0xb, 0, r);
        if (r[1] & 0xffff)
            return r[3];
    }
    cpuid_full(1, 0, r);
    return r[1] >> 24;
}

static unsigned smt_shift(void) {
    unsigned r[4], ml = max_leaf();
    if (ml >= 0xb) {
        cpuid_full(0xb, 0, r);
        if ((r[1] & 0xffff) && ((r[2] >> 8) & 0xff) == 1) /* level 0 = SMT */
            return r[0] & 31;
    }
    cpuid_full(1, 0, r);
    if (!(r[3] & (1u << 28)) || ml < 4)
        return 0;
    unsigned logical = (r[1] >> 16) & 0xff;
    cpuid_full(4, 0, r);
    unsigned cores = (r[0] >> 26) + 1;
    unsigned per   = cores ? logical / cores : 1, sh = 0;
    while ((1u << sh) < per)
        sh++;
    return sh;
}

static uint64_t tsc64(void) {
    uint32_t v[2];
    rdtsc2(v);
    return ((uint64_t) v[1] << 32) | v[0];
}

static void send_ipi(uint32_t lo) {
    send_ipi_to(0, lo);
}

/* Targeted start: wake only the first hyperthread of every physical core,
 * not the main core's own core; all other logical CPUs stay in the state
 * the BIOS left them in. Waking every CPU by broadcast would also start
 * Hyper-Threading siblings that are never used. Candidates come from the
 * ACPI MADT (enabled local APICs), else from the topology (CPUID 0Bh: SMT
 * and core bits; IDs that do not exist accept no IPI), else broadcast
 * (-1). The MADT comes first because firmware may own a CPU: CSMWrap (a
 * legacy BIOS on UEFI-only machines) keeps one AP as its BIOS proxy for
 * V86 mode and hides it from the MADT and MP table; an INIT to it would
 * stop the disk and keyboard. */
static unsigned g_targets[16];
static int      g_ntargets = -1;

/* Copies physical memory for reading ACPI tables: directly in ring 0
 * (no paging), through DPMI 0800h in ring 3 -- VDPMI maps 1:1 but only
 * what was requested. One mapping at a time: HDPMI hands out the same
 * linear page for the same physical page, and unmapping one would unmap
 * both. */
static int phys_copy(uint32_t phys, void *dst, uint32_t len) {
    uint8_t       *d = (uint8_t *) dst;
    const uint8_t *src;
    void          *m = NULL;
    if (g_mode == MODE_RING0) {
        src = (const uint8_t *) phys;
    } else {
        uint32_t off = phys & 0xfff;
        m            = r3_map_phys(phys - off, (off + len + 0xfff) & ~0xfffu);
        if (!m)
            return 0;
        src = (const uint8_t *) m + off;
    }
    for (uint32_t i = 0; i < len; i++)
        d[i] = src[i];
    if (m)
        r3_unmap(m);
    return 1;
}

static int sum8(const uint8_t *p, uint32_t n) {
    uint8_t s = 0;
    while (n--)
        s += *p++;
    return s == 0;
}

/* RSDP: first KB of the EBDA, then E0000h-FFFFFh (identity-mapped below
 * 1 MB in every mode used here) */
static const uint8_t *find_rsdp(void) {
    uint32_t ebda = (uint32_t) *(const uint16_t *) 0x40e << 4;
    uint32_t from[2], to[2];
    from[0] = ebda;
    to[0]   = ebda + 1024;
    from[1] = 0xe0000;
    to[1]   = 0x100000;
    for (int k = ebda >= 0x80000 && ebda < 0xa0000 ? 0 : 1; k < 2; k++)
        for (uint32_t a = from[k]; a + 20 <= to[k]; a += 16) {
            const uint8_t *p = (const uint8_t *) a;
            if (p[0] == 'R' && p[1] == 'S' && p[2] == 'D' && p[3] == ' ' && p[4] == 'P' && p[5] == 'T' &&
                p[6] == 'R' && p[7] == ' ' && sum8(p, 20))
                return p;
        }
    return NULL;
}

/* Enabled local APICs from the MADT ("APIC"), read once: number of IDs,
 * -1 without MADT */
#define MADT_MAX 64
static unsigned g_madt_ids[MADT_MAX];
static int      g_madt_n = -2; /* -2 not read yet */

static int madt_scan(uint8_t *buf, uint32_t bufsize);

static int madt_apics(void) {
    if (g_madt_n != -2)
        return g_madt_n;
    g_madt_n     = -1;
    uint8_t *buf = (uint8_t *) platform_alloc(4096);
    if (buf) {
        madt_scan(buf, 4096);
        platform_free(buf);
    }
    return g_madt_n;
}

static int madt_scan(uint8_t *buf, uint32_t bufsize) {
    uint32_t       ent[64], hdr[2];
    const uint8_t *rsdp = find_rsdp();
    if (!rsdp)
        return -1;
    uint32_t rsdt = *(const uint32_t *) (rsdp + 16), esz = 4;
    if (rsdp[15] >= 2 && *(const uint32_t *) (rsdp + 28) == 0 && *(const uint32_t *) (rsdp + 24)) {
        rsdt = *(const uint32_t *) (rsdp + 24); /* XSDT below 4 GB */
        esz  = 8;
    }
    if (!phys_copy(rsdt, hdr, 8) || hdr[1] < 36 || hdr[1] > bufsize || !phys_copy(rsdt, buf, hdr[1]))
        return -1;
    int nent = 0;
    for (uint32_t o = 36; o + esz <= hdr[1] && nent < 64; o += esz)
        if (esz == 4 || !*(const uint32_t *) (buf + o + 4))
            ent[nent++] = *(const uint32_t *) (buf + o);
    for (int i = 0; i < nent; i++) {
        if (!phys_copy(ent[i], hdr, 8) || hdr[0] != 0x43495041u /* "APIC" */)
            continue;
        uint32_t mlen = hdr[1];
        if (mlen < 44 || mlen > bufsize || !phys_copy(ent[i], buf, mlen))
            continue;
        int n = 0;
        for (uint32_t e = 44; e + 2 <= mlen && buf[e + 1] >= 2 && e + buf[e + 1] <= mlen; e += buf[e + 1]) {
            unsigned id, flags;
            if (buf[e] == 0 && buf[e + 1] >= 8) { /* processor local APIC */
                id    = buf[e + 3];
                flags = *(const uint32_t *) (buf + e + 4);
            } else if (buf[e] == 9 && buf[e + 1] >= 16) { /* processor local x2APIC */
                id    = *(const uint32_t *) (buf + e + 4);
                flags = *(const uint32_t *) (buf + e + 8);
            } else {
                continue;
            }
            if ((flags & 1) && n < MADT_MAX)
                g_madt_ids[n++] = id;
        }
        g_madt_n = n;
        break;
    }
    return g_madt_n;
}

/* From the MADT: the lowest enabled ID of every physical core except the
 * main core's own */
static int plan_from_madt(void) {
    int       n   = madt_apics();
    unsigned *ids = g_madt_ids;
    if (n < 0)
        return -1;
    unsigned sh = smt_shift(), bsp = my_apic_id(), k = 0;
    for (int i = 0; i < n; i++) {
        unsigned id = ids[i];
        if ((id >> sh) == (bsp >> sh) || (!g_x2apic && id > 0xff))
            continue;
        int dup = 0;
        for (unsigned j = 0; j < k; j++)
            if ((g_targets[j] >> sh) == (id >> sh)) {
                if (id < g_targets[j])
                    g_targets[j] = id;
                dup = 1;
            }
        if (!dup && k < 16)
            g_targets[k++] = id;
    }
    g_ntargets = (int) k;
#ifdef GLIDULATOR_TRACE
    {
        char  line[160];
        char *p = put_s(line, "Multi-core: targets from the MADT (");
        p       = put_dec(p, (unsigned) n);
        p       = put_s(p, " enabled APICs):");
        for (unsigned j = 0; j < k; j++) {
            *p++ = ' ';
            p    = put_dec(p, g_targets[j]);
        }
        *p++ = '\n';
        *p   = 0;
        platform_log(line);
    }
#endif
    return (int) k;
}

static int plan_targets(void) {
    unsigned r[4];
    g_ntargets = -1;
    if (plan_from_madt() >= 0)
        return g_ntargets;
    if (max_leaf() < 0xb)
        return -1;
    cpuid_full(0xb, 0, r);
    if (((r[2] >> 8) & 0xff) != 1 || !(r[1] & 0xffff))
        return -1;
    unsigned sh = r[0] & 31;
    cpuid_full(0xb, 1, r);
    if (((r[2] >> 8) & 0xff) != 2 || !(r[1] & 0xffff))
        return -1;
    unsigned cs  = r[0] & 31;
    unsigned bsp = my_apic_id();
    unsigned pkg = bsp & ~((1u << cs) - 1);
    int      n   = 0;
    for (unsigned c = 0; c < (1u << (cs - sh)) && n < 16; c++) {
        unsigned id = pkg | (c << sh);
        if ((id >> sh) == (bsp >> sh))
            continue;
        if (!g_x2apic && id > 0xff)
            break;
        g_targets[n++] = id;
    }
    g_ntargets = n;
    return n;
}

static void ipi_all(uint32_t lo) {
    if (g_ntargets < 0) {
        send_ipi(ICR_ALL_BUT_SELF | lo);
        return;
    }
    for (int i = 0; i < g_ntargets; i++)
        send_ipi_to(g_targets[i], lo);
}

/* Every AP lands here (cdecl: the trampoline pushes idx). */
static void __cdecl ap_entry(int idx) {
    volatile ap_slot *s = &g_slot[idx];
    s->apic             = my_apic_id();
    s->ready            = 1;
    if (g_mode == MODE_PAGING)
        set_cr3(g_r3_cr3); /* TLB: mappings may have changed since the start */
    fldcw(&g_fpu_cw);
    if (g_have_mxcsr)
        ldmxcsr(&g_mxcsr);
    while (s->state != SLOT_JOB) {
        uint32_t ticks = g_park_ticks;
        if (ticks) {
            uint64_t rel = ((uint64_t) g_release[1] << 32) | g_release[0];
            if (tsc64() - rel > ticks && cas32(&s->state, SLOT_WAIT, SLOT_PARK) == SLOT_WAIT)
                park_forever();
        }
        cpu_pause();
    }
    s->fn(s->arg);
    s->done = 1;
    park_forever();
}

/* "hlt; jmp short hlt" (F4 EB FD) in the BIOS ROM (F0000h-FFFFFh), or 0.
 * Searched once. */
static uint32_t g_rom_hlt = 1;

static uint32_t rom_hlt(void) {
    static uint8_t buf[4096 + 2];
    if (g_rom_hlt != 1)
        return g_rom_hlt;
    g_rom_hlt = 0;
    for (uint32_t a = 0xf0000; a < 0x100000 && !g_rom_hlt; a += 4096) {
        uint32_t len = a + 4096 + 2 <= 0x100000 ? 4096 + 2 : 4096;
        if (!phys_copy(a, buf, len))
            break;
        for (uint32_t i = 0; i + 3 <= len; i++)
            if (buf[i] == 0xf4 && buf[i + 1] == 0xeb && buf[i + 2] == 0xfd) {
                g_rom_hlt = a + i;
                break;
            }
    }
    return g_rom_hlt;
}

/* A core in wait-for-SIPI (after INIT) does not join the firmware's SMI
 * rendezvous: every SMI then waits for the BIOS's timeout. With a USB
 * keyboard under DOS (legacy emulation, one SMI per port 60h/64h access)
 * that took 2.1 ms instead of 96 us per access on an i7-4770T, and
 * MS-DOS halted with "internal stack overflow" after a few key presses.
 * So the stopped APs get one more SIPI into a few bytes in the trampoline
 * page that jump to a hlt loop of the BIOS ROM: halted, but in a state
 * that answers SMIs, and nothing of ours stays in use. Each AP counts
 * itself there, so the page is freed only after all have left it. Without
 * such a loop in the ROM they stay in wait-for-SIPI. */
static void park_in_rom(void) {
    uint32_t hlt = rom_hlt();
    uint32_t n   = ((volatile tr_params *) (g_tr + P_BASE))->count;
    if (!hlt || !n)
        return;
    volatile uint8_t *c = g_tr;
    static const uint8_t code[12] = { 0xfa,                               /* cli */
                                      0x2e, 0xf0, 0xfe, 0x06, 0x10, 0x00, /* lock inc byte [cs:10h] */
                                      0xea, 0, 0, 0x00, 0xf0 };           /* jmp far F000:hlt */
    for (int i = 0; i < 12; i++)
        c[i] = code[i];
    c[8]    = (uint8_t) (hlt - 0xf0000);
    c[9]    = (uint8_t) ((hlt - 0xf0000) >> 8);
    c[0x10] = 0;
    for (int k = 0; k < 2 && c[0x10] < n; k++) {
        ipi_all(ICR_SIPI | ((uint32_t) g_tr >> 12));
        wait_us(200);
    }
    for (int t = 0; t < 100 && c[0x10] < n; t++)
        wait_us(100);
    if (c[0x10] < n)
        LOG_SMP("APs in the ROM hlt loop (fewer than started): ", c[0x10]);
}

static void stop_aps(void) {
    if (!g_running)
        return;
    ipi_all(ICR_ASSERT | ICR_INIT);
    wait_us(1000);
    park_in_rom();
    apic_restore(); /* the APIC as found */
#ifdef GLIDULATOR_DEBUG
    {   /* only if platform_worker_failed() has not reported it already */
        int seen = 0;
        for (int i = 0; i < AP_MAX; i++)
            seen |= g_slot[i].lost;
        if (((tr_params *) (g_tr + P_BASE))->exc_flag && !seen)
            log_exc((tr_params *) (g_tr + P_BASE));
    }
#endif
    dos_free(g_dos_sel);
    platform_free(g_stacks);
    g_tr      = NULL;
    g_stacks  = NULL;
    g_running = 0;
}

/* Starts all APs; the first AP_MAX then wait in ap_entry() for work.
 * Returns the number of APs that reported (0: none/error). */
static int start_aps(void) {
    unsigned seg;
    if (g_running)
        return g_count;
    if (!apic_enable()) {
        LOG_SMP("off (APIC cannot be switched on again)", -1);
        return 0;
    }
    /* 4 KB page below 1 MB: allocate 8 KB, align inside */
    seg = dos_alloc(0x200, &g_dos_sel);
    if (!seg) {
        apic_restore();
        LOG_SMP("off (no DOS memory)", -1);
        return 0;
    }
    uint32_t page = ((uint32_t) seg * 16 + 0xfff) & ~0xfffu;
    g_stacks      = (uint8_t *) platform_alloc(AP_STACK * AP_MAX + 16);
    if (!g_stacks || page + TR_BYTES > 0xa0000 || (g_mode == MODE_PAGING && !identity_mapped(page))) {
        if (g_stacks)
            LOG_SMP("off (trampoline page not identity-mapped)", -1);
        platform_free(g_stacks);
        dos_free(g_dos_sel);
        apic_restore();
        return 0;
    }
    g_tr = (uint8_t *) page;
    for (unsigned i = 0; i < TR_BYTES; i++)
        g_tr[i] = i < sizeof ap_tramp_bin ? ap_tramp_bin[i] : 0;

    tr_params *pr = (tr_params *) (g_tr + P_BASE);
    pr->idt_limit = 32 * 8 - 1;
    pr->idt_base  = page + TR_IDT;
    pr->jmp_off   = page + TR_PM32;
    if (g_mode == MODE_RING0) {
        sgdt(&pr->gdt_limit);
        pr->jmp_sel = (uint16_t) get_cs();
        pr->dsel    = (uint16_t) get_ds();
        pr->cr0     = get_cr0();
        pr->cr4     = get_cr4();
        pr->cr3     = 0;
    } else {
        /* flat ring-0 code and data, base 0 like the client's selectors */
        static const uint8_t desc[24] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0x9a, 0xcf, 0,
                                          0xff, 0xff, 0, 0, 0, 0x92, 0xcf, 0 };
        for (int i = 0; i < 24; i++)
            g_tr[TR_GDT + i] = desc[i];
        pr->gdt_limit = 23;
        pr->gdt_base  = page + TR_GDT;
        pr->jmp_sel   = 0x08;
        pr->dsel      = 0x10;
        if (g_mode == MODE_PAGING) {
            pr->cr0 = g_r3_cr0;
            pr->cr4 = g_r3_cr4;
            pr->cr3 = g_r3_cr3;
        } else { /* PE, MP, ET, NE; SSE for the AP itself if the CPU has it */
            pr->cr0 = 0x33;
            pr->cr4 = (cpuid1_edx() & (1u << 24)) ? CR4_OSFXSR : 0;
            pr->cr3 = 0;
        }
    }
#ifdef GLIDULATOR_DEBUG
    /* Machine check as exception 18 (the stub saves the banks) instead of a
     * reset of the whole machine; diag_dos.c enables it on the main core */
    if (cpuid1_edx() & (1u << 7))
        pr->cr4 |= 0x40;
#endif
    pr->count     = 0;
    pr->max       = AP_MAX;
    pr->stack     = ((uint32_t) g_stacks + 15) & ~15u;
    pr->stsize    = AP_STACK;
    pr->entry     = (uint32_t) ap_entry;
    pr->exc_flag  = 0;
    pr->exc_esp   = 0;
    pr->mc_n      = 0;
    for (int b = 0; b < 10; b++)
        pr->mc[b][0] = pr->mc[b][1] = pr->mc[b][2] = pr->mc[b][3] = 0;
    tr_guard *gd       = (tr_guard *) (g_tr + TR_GUARD);
    gd->heap           = pr->stack;
    gd->magic          = (uint32_t) tsc64() | 1u;
    *(uint32_t *) pr->stack = gd->magic;
    gd->code0          = ((const uint32_t *) (const void *) ap_entry)[0];
    gd->code1          = ((const uint32_t *) (const void *) ap_entry)[1];
    gd->bad            = 0;
#ifdef GLIDULATOR_DEBUG
    if (smp_test() == 2) {
        LOG_SMP("Test: wrong check value for the guard (GLIDULATOR_SMP_TEST=magic)", -1);
        gd->magic ^= 1;
    }
#endif
    for (int v = 0; v < 32; v++) {
        uint8_t *g = g_tr + TR_IDT + 8 * v;
        uint32_t o = page + TR_STUBS + 8 * v;
        g[0]       = (uint8_t) o;
        g[1]       = (uint8_t) (o >> 8);
        g[2]       = (uint8_t) pr->jmp_sel;
        g[3]       = (uint8_t) (pr->jmp_sel >> 8);
        g[4]       = 0;
        g[5]       = 0x8e; /* 32-bit interrupt gate, ring 0 */
        g[6]       = (uint8_t) (o >> 16);
        g[7]       = (uint8_t) (o >> 24);
    }
    /* take over the main core's FPU/SSE settings (bit-identical results) */
    fnstcw(&g_fpu_cw);
    /* the main core may lack SSE in ring 3 (VDPMI) even if the APs have it */
    g_have_mxcsr = (pr->cr4 & CR4_OSFXSR) && (g_mode == MODE_RING0 || (platform_cpu_features() & PLATFORM_CPU_SSE));
    if (g_have_mxcsr)
        stmxcsr(&g_mxcsr);
    for (int i = 0; i < AP_MAX; i++) {
        g_slot[i].fn    = NULL;
        g_slot[i].state = SLOT_WAIT;
        g_slot[i].done  = 0;
        g_slot[i].ready = 0;
        g_slot[i].lost  = 0;
    }
    g_norder     = 0;
    g_park_ticks = 0;
    g_running    = 1;

    plan_targets();
    ipi_all(ICR_ASSERT | ICR_INIT);
    uint64_t c0 = tsc64();
    wait_us(10000);
    uint64_t per_ms = (tsc64() - c0) / 10;
    for (int k = 0; k < 2; k++) {
        ipi_all(ICR_SIPI | (page >> 12));
        wait_us(200);
    }
    /* APs report within microseconds; the count must stay stable for 20 ms */
    uint32_t n = 0;
    for (int stable = 0; stable < 20;) {
        wait_us(1000);
        uint32_t c = *(volatile uint32_t *) &pr->count;
        stable     = (c == n) ? stable + 1 : 0;
        n          = c;
    }
    if (pr->exc_flag) {
#ifdef GLIDULATOR_DEBUG
        log_exc(pr);
#endif
        stop_aps();
        g_state = -1;
        return 0;
    }
    if (gd->bad) {
        LOG_SMP("off (guard: wrong address space on the APs), halted: ", (int) gd->bad);
        stop_aps();
        g_state = -1;
        return 0;
    }
    /* One AP per physical core, excluding the main core's core */
    unsigned sh   = smt_shift();
    unsigned bsp  = my_apic_id() >> sh;
    unsigned used[AP_MAX];
    int      nord = 0;
    for (int i = 0; i < AP_MAX && i < (int) n; i++) {
        if (!g_slot[i].ready)
            continue;
        unsigned core = g_slot[i].apic >> sh;
        int      dup  = core == bsp;
        for (int k = 0; k < nord && !dup; k++)
            dup = used[k] == core;
        if (dup)
            continue;
        used[nord]      = core;
        g_order[nord++] = i;
    }
    g_norder = nord;
    /* unused APs halt 200 ms from now */
    uint64_t now = tsc64();
    g_release[0] = (uint32_t) now;
    g_release[1] = (uint32_t) (now >> 32);
    g_park_ticks = (uint32_t) (per_ms * 200 > 0xffffffffu ? 0xffffffffu : per_ms * 200);
#ifdef GLIDULATOR_TRACE
    {
        char  line[160];
        char *p = put_s(line, "Multi-core: ");
        p       = put_dec(p, n);
        p       = put_s(p, g_ntargets < 0 ? " APs (broadcast), SMT bits " : " APs (targeted), SMT bits ");
        p       = put_dec(p, sh);
        p       = put_s(p, ", BSP APIC ");
        p       = put_dec(p, my_apic_id());
        p       = put_s(p, ", AP APICs:");
        for (int i = 0; i < AP_MAX && i < (int) n; i++) {
            *p++ = ' ';
            p    = put_dec(p, g_slot[i].apic);
        }
        *p++ = '\n';
        *p   = 0;
        platform_log(line);
    }
#endif
    return nord;
}

/* Emergency (crash on the main core, diag_dos.c): stop all APs with INIT
 * without waiting for workers. */
void smp_dos_emergency_stop(void) {
    if (!g_running)
        return;
#ifdef GLIDULATOR_DEBUG
    if (((tr_params *) (g_tr + P_BASE))->exc_flag)
        log_exc((tr_params *) (g_tr + P_BASE)); /* the extra cores may have seen something too */
#endif
    ipi_all(ICR_ASSERT | ICR_INIT);
    if (g_rom_hlt != 1) { /* no ROM search (DPMI calls) on the crash path */
        wait_us(1000);
        park_in_rom();
    }
    apic_restore();
}

int platform_core_count(void) {
    char cfg[8];
    if (g_count < 0) {
        g_count = 0;
        if (platform_get_config("GLIDULATOR_THREADS", cfg, sizeof cfg) && cfg[0] == '1' && !cfg[1]) {
            LOG_SMP("off (GLIDULATOR_THREADS=1)", -1);
        } else if (check()) {
            g_count = start_aps();
            stop_aps();
            LOG_SMP("usable additional cores (one per physical core): ", g_count);
        }
    }
    return 1 + g_count;
}

platform_worker *platform_spawn_worker(int core_id, platform_worker_fn fn, void *arg) {
    int w = core_id - 1;
    if (platform_core_count() < 2 || w < 0 || w >= g_count)
        return NULL;
    if (!g_running)
        start_aps();
    if (w >= g_norder) {
        if (!g_active)
            stop_aps();
        return NULL;
    }
    int               idx = g_order[w];
    volatile ap_slot *s   = &g_slot[idx];
    s->fn                 = fn;
    s->arg                = arg;
    if (cas32(&s->state, SLOT_WAIT, SLOT_JOB) != SLOT_WAIT) { /* already taken/halted */
        if (!g_active)
            stop_aps();
        return NULL;
    }
    g_active++;
    g_worker[w].idx = idx;
    LOG_SMP("worker on AP slot ", idx);
    return &g_worker[w];
}

/* Exception on this worker's core? The trampoline records the stack at
 * the fault; every AP has its own (AP_STACK each from pr->stack). */
int platform_worker_failed(platform_worker *w) {
    if (!w || !g_running)
        return 0;
    volatile ap_slot   *s  = &g_slot[w->idx];
    volatile tr_params *pr = (volatile tr_params *) (g_tr + P_BASE);
    if (s->lost)
        return 1;
    if (!pr->exc_flag)
        return 0;
    uint32_t esp = pr->exc_esp;
    if (esp > pr->stack && (int) ((esp - pr->stack - 1) / AP_STACK) == w->idx) {
        s->lost = 1;
#ifdef GLIDULATOR_DEBUG
        log_exc((const tr_params *) pr);
#endif
        return 1;
    }
    return 0;
}

int platform_kill_worker(platform_worker *w) {
    if (!w || !g_running)
        return 0;
    g_slot[w->idx].lost = 1; /* keeps running until stop_aps() sends INIT */
    LOG_SMP("worker abandoned (hangs), AP slot ", w->idx);
    return 1;
}

void platform_join_worker(platform_worker *w) {
    if (!w)
        return;
    volatile ap_slot *s = &g_slot[w->idx];
    while (!s->done && !s->lost && !platform_worker_failed(w))
        cpu_pause();
    if (--g_active == 0)
        stop_aps();
}
