/* Hardware diagnostics of the debug OVL (GLIDULATOR_DEBUG only, ring 0):
 *
 *  - ovl_diag_machine(): the CPU's machine-check registers (MCG_STATUS, all
 *    banks with a valid entry) and CR4.MCE to the log. Banks survive a warm
 *    boot, so a run after a "machine check" crash shows at start what the
 *    CPU reported.
 *  - Exceptions 0 (divide), 6 (invalid opcode), 13 (protection fault) and
 *    18 (machine check) on the main core are handled by the OVL itself
 *    between "Start" and "End" (own IDT entries): registers, code address
 *    (tools/dosvm/ovladdr.py) and machine-check banks to the log, other
 *    cores stopped, MTRRs/CR4 restored, text mode, program exit. Otherwise
 *    DOS/4GW prints its register dump in graphics mode (unreadable).
 *
 * MSRs are read only if CPUID announces them (an unknown MSR raises #GP). */

#include "glidulator/platform.h"

#include "host_memory.h"

void ovl_diag_machine(const char *when);

#ifndef GLIDULATOR_DEBUG

void ovl_diag_machine(const char *when) {
    (void) when;
}

#else

static unsigned d_get_cs(void);
#pragma aux d_get_cs = "mov ax, cs" "movzx eax, ax" value [eax];
static unsigned d_get_cr4(void);
#pragma aux d_get_cr4 = 0x0f 0x20 0xe0 value [eax];
static unsigned d_eflags_id(void);
#pragma aux d_eflags_id = \
    "pushfd" "pop eax" "mov ecx, eax" "xor eax, 200000h" "push eax" "popfd" \
    "pushfd" "pop eax" "push ecx" "popfd" "xor eax, ecx" value [eax] modify [ecx];
static void d_cpuid(unsigned leaf, unsigned *out);
#pragma aux d_cpuid = \
    "push ebx" "xor ecx, ecx" 0x0f 0xa2 "mov [edi], eax" "mov [edi+4], ebx" "mov [edi+8], ecx" \
    "mov [edi+12], edx" "pop ebx" parm [eax] [edi] modify [eax ecx edx];
static void d_rdmsr(unsigned msr, unsigned *lo, unsigned *hi);
#pragma aux d_rdmsr = 0x0f 0x32 "mov [esi], eax" "mov [edi], edx" parm [ecx] [esi] [edi] modify [eax edx];

static int      g_ok = -1; /* -1 unchecked, 0 no, 1 ring 0 with CPUID */
static unsigned g_edx1;

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

static char *put_dec(char *p, int v) {
    char t[12];
    int  k = 0;
    if (v < 0) {
        *p++ = '-';
        v    = -v;
    }
    do
        t[k++] = (char) ('0' + v % 10);
    while ((v /= 10) && k < 11);
    while (k)
        *p++ = t[--k];
    return p;
}

static int ready(void) {
    unsigned r[4];
    if (g_ok >= 0)
        return g_ok;
    g_ok = 0;
    if (platform_host_memory_set() || (d_get_cs() & 3) || !(d_eflags_id() & 0x200000u))
        return 0;
    d_cpuid(1, r);
    g_edx1 = r[3];
    if (!(g_edx1 & (1u << 5))) /* nothing without MSRs */
        return 0;
    g_ok = 1;
    return 1;
}

/* ---- Exceptions on the main core ------------------------------------ */

void smp_dos_emergency_stop(void); /* smp_dos.c */
void platform_cpu_release(void);   /* platform_watcom.c */
void wc_dos_off_crash(void);       /* wc_dos.c */

/* Own entries in the CPU's IDT (ring 0 without paging: the IDT base from
 * SIDT is directly addressable). DPMI 0203h does not work for this:
 * DOS/4GW handles ring-0 exceptions itself (register dump in graphics
 * mode, exit without stopping the other cores). Vector 13 (protection
 * fault) is shared with IRQ 5 under DOS/4GW: if IRQ 5 is in the PIC's
 * in-service register, the old handler is chained, otherwise it is the
 * exception (with error code). */
static const unsigned char g_vecs[4] = { 0, 6, 18, 13 };
static unsigned char       g_old_gate[4][8];
static unsigned            g_old13[2]; /* offset, selector: jump target for IRQ 5 */
static unsigned char      *g_idt;
static int                 g_hooked;
/* filled by the entry stub */
static unsigned x_vec, x_eip, x_cs, x_err, x_efl, x_esp, x_eax, x_ebx, x_ecx, x_edx, x_esi, x_edi, x_ebp;

static void crash_continue(void);

/* Ring-0 frame without stack switch: [esp] EIP, CS, EFLAGS. Save the
 * registers, redirect the return to crash_continue, iretd. */
static void __declspec(naked) exc_common(void) {
    _asm {
        mov x_ebx, ebx
        mov x_ecx, ecx
        mov x_edx, edx
        mov x_esi, esi
        mov x_edi, edi
        mov x_ebp, ebp
        mov x_vec, eax
        pop eax
        mov x_eax, eax
        mov eax, [esp]
        mov x_eip, eax
        mov eax, [esp+4]
        mov x_cs, eax
        mov eax, [esp+8]
        mov x_efl, eax
        lea eax, [esp+12]
        mov x_esp, eax
        mov eax, offset crash_continue
        mov [esp], eax
        and dword ptr [esp+8], 0FFFFFEFFh
        iretd
    }
}
static void __declspec(naked) exc0(void) {
    _asm { push eax
           mov eax, 0
           jmp exc_common }
}
static void __declspec(naked) exc6(void) {
    _asm { push eax
           mov eax, 6
           jmp exc_common }
}
static void __declspec(naked) exc13(void) {
    _asm {
        push eax
        mov al, 0Bh
        out 20h, al
        in al, 20h
        mov ah, al
        mov al, 0Ah
        out 20h, al
        test ah, 20h
        jz gp
        pop eax
        jmp fword ptr g_old13
    gp:
        mov eax, [esp+4]
        mov x_err, eax
        pop eax
        add esp, 4
        push eax
        mov eax, 13
        jmp exc_common
    }
}
static void __declspec(naked) exc18(void) {
    _asm { push eax
           mov eax, 18
           jmp exc_common }
}

static void d_sidt(void *p);
#pragma aux d_sidt = 0x0f 0x01 0x08 parm [eax]; /* sidt [eax] */
static unsigned d_get_cs16(void);
#pragma aux d_get_cs16 = "xor eax, eax" "mov ax, cs" value [eax];
static unsigned d_cli(void);
#pragma aux d_cli = "pushfd" "pop eax" "cli" value [eax];
static void d_restore(unsigned f);
#pragma aux d_restore = "push eax" "popfd" parm [eax];
static void text_mode(void);
#pragma aux text_mode = "mov ax, 0003h" "int 10h" modify [eax ebx ecx edx esi edi];
static void dos_exit(void);
#pragma aux dos_exit = "mov ax, 4C03h" "int 21h" aborts;
static void dos_print(const char *s);
#pragma aux dos_print = "mov ah, 09h" "int 21h" parm [edx] modify [eax];
static void d_set_cr4(unsigned v);
#pragma aux d_set_cr4 = 0x0f 0x22 0xe0 parm [eax];
static int g_mce_set; /* CR4.MCE set here (otherwise a machine check resets instead of raising #MC) */
static void d_wrmsr(unsigned msr, unsigned lo, unsigned hi);
#pragma aux d_wrmsr = 0x0f 0x30 parm [ecx] [eax] [edx];

static void hook_exceptions(void) {
    unsigned char r[6];
    if (g_hooked)
        return;
    d_sidt(r);
    unsigned limit = r[0] | (r[1] << 8);
    g_idt          = (unsigned char *) (r[2] | (r[3] << 8) | (r[4] << 16) | ((unsigned) r[5] << 24));
    if (limit < 19 * 8 - 1)
        return;
    void (*h[4])(void) = { exc0, exc6, exc18, exc13 };
    {
        const unsigned char *o = g_idt + 8 * 13;
        g_old13[0] = o[0] | (o[1] << 8) | (o[6] << 16) | ((unsigned) o[7] << 24);
        g_old13[1] = o[2] | (o[3] << 8);
    }
    unsigned cs = d_get_cs16();
    /* Which vectors are hooked (for tests): GLIDULATOR_DIAG_VEC = bit mask
     * 1=0, 2=6, 4=18, 8=13 (default all) */
    unsigned mask = 15;
    {
        char cfg[8];
        if (platform_get_config("GLIDULATOR_DIAG_VEC", cfg, sizeof cfg) && cfg[0] >= '0' && cfg[0] <= '9')
            mask = (unsigned) (cfg[0] - '0') + (cfg[1] >= '0' && cfg[1] <= '9' ? 0 : 0);
    }
    /* log the original gates (selector, offset, type) */
    for (int v = 0; v < 20 && v * 8 + 7 <= (int) limit; v++) {
        const unsigned char *o = g_idt + 8 * v;
        char                 line[80], *p = put_s(line, "IDT ");
        p = put_dec(p, v);
        p = put_s(p, ": ");
        p = put_hex(p, o[2] | (o[3] << 8));
        *p++ = ':';
        p = put_hex(p, o[0] | (o[1] << 8) | (o[6] << 16) | ((unsigned) o[7] << 24));
        p = put_s(p, " type ");
        p = put_hex(p, o[5]);
        *p++ = '\n';
        *p   = 0;
        platform_log(line);
    }
    unsigned f = d_cli();
    for (int i = 0; i < 4; i++) {
        unsigned char *g = g_idt + 8 * g_vecs[i];
        unsigned       o = (unsigned) h[i];
        for (int k = 0; k < 8; k++)
            g_old_gate[i][k] = g[k];
        if (!(mask & (1u << i)))
            continue;
        g[0] = (unsigned char) o;
        g[1] = (unsigned char) (o >> 8);
        g[2] = (unsigned char) cs;
        g[3] = (unsigned char) (cs >> 8);
        g[4] = 0;
        g[5] = 0x8e; /* 32-bit interrupt gate, ring 0 */
        g[6] = (unsigned char) (o >> 16);
        g[7] = (unsigned char) (o >> 24);
    }
    if ((g_edx1 & (1u << 7)) && !(d_get_cr4() & 0x40)) {
        d_set_cr4(d_get_cr4() | 0x40);
        g_mce_set = 1;
    }
    d_restore(f);
    g_hooked = 1;
    platform_log("Exceptions 0/6/13/18 of the main core: own handling on\n");
}

static void unhook_exceptions(void) {
    if (!g_hooked)
        return;
    unsigned f = d_cli();
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 8; k++)
            g_idt[8 * g_vecs[i] + k] = g_old_gate[i][k];
    if (g_mce_set) {
        d_set_cr4(d_get_cr4() & ~0x40u);
        g_mce_set = 0;
    }
    d_restore(f);
    g_hooked = 0;
}

static void log_regs(void) {
    char  line[160];
    char *p = put_s(line, "CRASH main core: exception ");
    p       = put_dec(p, (int) x_vec);
    p       = put_s(p, " at EIP ");
    p       = put_hex(p, x_eip);
    p       = put_s(p, " CS ");
    p       = put_hex(p, x_cs & 0xffff);
    p       = put_s(p, " error code ");
    p       = put_hex(p, x_err);
    p       = put_s(p, " EFLAGS ");
    p       = put_hex(p, x_efl);
    *p++    = '\n';
    *p      = 0;
    platform_log(line);
    p = put_s(line, "  EAX ");
    p = put_hex(p, x_eax);
    p = put_s(p, " EBX ");
    p = put_hex(p, x_ebx);
    p = put_s(p, " ECX ");
    p = put_hex(p, x_ecx);
    p = put_s(p, " EDX ");
    p = put_hex(p, x_edx);
    p = put_s(p, " ESI ");
    p = put_hex(p, x_esi);
    p = put_s(p, " EDI ");
    p = put_hex(p, x_edi);
    p = put_s(p, " EBP ");
    p = put_hex(p, x_ebp);
    p = put_s(p, " ESP ");
    p = put_hex(p, x_esp);
    *p++ = '\n';
    *p   = 0;
    platform_log(line);
}

void ovl_diag_machine(const char *when);

static void crash_continue(void) {
    log_regs();
    ovl_diag_machine("Crash");
    unhook_exceptions();
    if (x_vec == 18 && (g_edx1 & (1u << 14)))
        d_wrmsr(0x17a, 0, 0); /* clear MCG_STATUS.MCIP, otherwise the next machine check resets */
    smp_dos_emergency_stop();
    wc_dos_off_crash();
    platform_cpu_release();
    platform_log("CRASH: cores stopped, MTRRs/CR4 restored, exiting\n");
    text_mode();
    dos_print("Glidulator (trace OVL): crash, details in GLIDULAT.LOG\r\n$");
    dos_exit();
}

void ovl_diag_machine(const char *when) {
    char     line[160], *p;
    unsigned lo, hi;
    if (!ready())
        return;
    if (when[0] == 'S')
        hook_exceptions();
    else if (when[0] == 'E')
        unhook_exceptions();
    p = put_s(line, "Machine Check (");
    p = put_s(p, when);
    p = put_s(p, "): CR4.MCE ");
    p = put_dec(p, (int) ((d_get_cr4() >> 6) & 1));
    if (!(g_edx1 & (1u << 14)) || !(g_edx1 & (1u << 7))) {
        p = put_s(p, ", CPU without MCA\n");
        *p = 0;
        platform_log(line);
        return;
    }
    d_rdmsr(0x179, &lo, &hi); /* MCG_CAP */
    unsigned banks = lo & 0xff;
    p              = put_s(p, ", banks ");
    p              = put_dec(p, (int) banks);
    d_rdmsr(0x17a, &lo, &hi); /* MCG_STATUS */
    p = put_s(p, ", MCG_STATUS ");
    p = put_hex(p, lo);
    *p++ = '\n';
    *p   = 0;
    platform_log(line);
    int any = 0;
    for (unsigned b = 0; b < banks && b < 32; b++) {
        d_rdmsr(0x401 + 4 * b, &lo, &hi); /* MCi_STATUS */
        if (!(hi & 0x80000000u))          /* VAL */
            continue;
        any = 1;
        p   = put_s(line, "  Bank ");
        p   = put_dec(p, (int) b);
        p   = put_s(p, ": STATUS ");
        p   = put_hex(p, hi);
        p   = put_hex(p, lo);
        if (hi & (1u << 29)) /* UC */
            p = put_s(p, " uncorrected");
        if (hi & (1u << 25)) /* PCC */
            p = put_s(p, " state corrupt");
        if (hi & (1u << 26)) { /* ADDRV */
            unsigned alo, ahi;
            d_rdmsr(0x402 + 4 * b, &alo, &ahi);
            p = put_s(p, " ADDR ");
            p = put_hex(p, ahi);
            p = put_hex(p, alo);
        }
        if (when[0] == 'S') /* stale entry (warm boot): clear it so a new error
                             * clearly belongs to this run */
            d_wrmsr(0x401 + 4 * b, 0, 0);
        if (hi & (1u << 27)) { /* MISCV */
            unsigned mlo, mhi;
            d_rdmsr(0x403 + 4 * b, &mlo, &mhi);
            p = put_s(p, " MISC ");
            p = put_hex(p, mhi);
            p = put_hex(p, mlo);
        }
        *p++ = '\n';
        *p   = 0;
        platform_log(line);
    }
    if (!any)
        platform_log("  no valid entries\n");
}

#endif
