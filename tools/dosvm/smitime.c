/* SMI timing probe: do the other cores' states slow down the firmware's
 * system management interrupts (SMIs)? A USB keyboard under DOS runs
 * through the BIOS's legacy emulation, which is SMI code; on many boards
 * every SMI waits until all cores have entered SMM. A core in
 * wait-for-SIPI (after our INIT) may not answer, so each SMI would run
 * into the firmware's timeout.
 *
 *   SMITIME          measure only
 *   SMITIME INIT     INIT to all other cores (they wait for a SIPI, as
 *                    after the OVL's workers), then measure
 *   SMITIME ROM      INIT + SIPI into a "hlt; jmp" loop found in the BIOS
 *                    ROM (cores halted but awake for SMIs), then measure
 *
 * Measurement: 1 s of reading the TSC (gaps over 50 us are counted: SMIs,
 * interrupts), 32 reads each of ports 64h and 60h (I/O traps of the legacy
 * emulation), MSR 34h (SMI count, ring 0 only). Ring 0 (DOS/4GW without
 * host) or ring 3 under HDPMI (APIC via DPMI 0800h). Output appended to
 * SMITIME.TXT, closed after every line. Built by tools/dosvm/run.sh. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <i86.h>

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

static void out(const char *fmt, ...) {
    va_list a;
    FILE   *f = fopen("SMITIME.TXT", "a");
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fclose(f);
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
}

static unsigned get_cs(void);
#pragma aux get_cs = "xor eax, eax" "mov ax, cs" value [eax];
static void rdtsc2(u32 *v);
#pragma aux rdtsc2 = 0x0f 0x31 "mov [ecx], eax" "mov [ecx+4], edx" parm [ecx] modify [eax edx];
static void rdmsr2(unsigned msr, unsigned *lo, unsigned *hi);
#pragma aux rdmsr2 = "xor eax, eax" "xor edx, edx" 0x0f 0x32 "mov [esi], eax" "mov [edi], edx" parm [ecx] [esi] [edi] modify [eax edx];
static void cpuid4(unsigned leaf, unsigned sub, unsigned *r);
#pragma aux cpuid4 = "push ebx" 0x0f 0xa2 "mov [edi], eax" "mov [edi+4], ebx" "mov [edi+8], ecx" "mov [edi+12], edx" "pop ebx" parm [eax] [ecx] [edi] modify [eax ecx edx];
static void cpu_pause(void);
#pragma aux cpu_pause = 0xf3 0x90;

static unsigned long long tsc(void) {
    u32 v[2];
    rdtsc2(v);
    return ((unsigned long long) v[1] << 32) | v[0];
}

static u32 g_per_us; /* TSC ticks per microsecond */

static void calibrate(void) {
    volatile u32 *tick = (volatile u32 *) 0x46c;
    u32           t    = *tick;
    while (*tick == t)
        ;
    unsigned long long a = tsc();
    t                    = *tick;
    while (*tick - t < 4)
        ;
    g_per_us = (u32) ((tsc() - a) / 219700); /* 4 ticks = 219.7 ms */
    if (!g_per_us)
        g_per_us = 1;
}

static void wait_us(u32 us) {
    unsigned long long t0 = tsc(), n = (unsigned long long) g_per_us * us;
    while (tsc() - t0 < n)
        cpu_pause();
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

static volatile u32 *g_apic;
static int           g_ring0;

static void ipi_all(u32 lo) { /* all excluding self */
    g_apic[0x310 / 4] = 0;
    g_apic[0x300 / 4] = 0xc0000u | lo;
    for (int i = 0; i < 100000 && (g_apic[0x300 / 4] & 0x1000u); i++)
        cpu_pause();
}

static void measure(const char *label) {
    u32 smi0 = 0, smi1 = 0, hi;
    if (g_ring0)
        rdmsr2(0x34, &smi0, &hi);
    unsigned long long end = tsc() + (unsigned long long) g_per_us * 1000000u, last = tsc(), now;
    u32 thr = g_per_us * 50, n = 0, maxg = 0;
    unsigned long long sum = 0;
    while ((now = tsc()) < end) {
        u32 d = (u32) (now - last);
        if (d > thr) {
            n++;
            sum += d;
            if (d > maxg)
                maxg = d;
        }
        last = now;
    }
    if (g_ring0)
        rdmsr2(0x34, &smi1, &hi);
    out("%s: 1 s: %u gaps > 50 us, %u us in gaps, longest %u us", label, n, (u32) (sum / g_per_us),
        maxg / g_per_us);
    if (g_ring0)
        out(", SMIs %u", smi1 - smi0);
    out("\n");
    for (int port = 0x64; port >= 0x60; port -= 4) {
        u32 mn = 0xffffffffu, mx = 0;
        unsigned long long tot = 0;
        if (g_ring0)
            rdmsr2(0x34, &smi0, &hi);
        for (int i = 0; i < 32; i++) {
            unsigned long long a = tsc();
            (void) inp(port);
            u32 d = (u32) (tsc() - a);
            tot += d;
            if (d < mn)
                mn = d;
            if (d > mx)
                mx = d;
        }
        if (g_ring0)
            rdmsr2(0x34, &smi1, &hi);
        out("  32x in %02xh: min %u us, avg %u us, max %u us", port, mn / g_per_us, (u32) (tot / 32 / g_per_us),
            mx / g_per_us);
        if (g_ring0)
            out(", SMIs %u", smi1 - smi0);
        out("\n");
    }
}

/* "hlt; jmp short hlt" (F4 EB FD) in the BIOS ROM, F0000-FFFFF */
static u32 find_rom_hlt(void) {
    const u8 *p = (const u8 *) 0xf0000;
    for (u32 i = 0; i + 3 <= 0x10000; i++)
        if (p[i] == 0xf4 && p[i + 1] == 0xeb && p[i + 2] == 0xfd)
            return 0xf0000 + i;
    return 0;
}

int main(int argc, char **argv) {
    unsigned r[4];
    int      mode = 0; /* 1 INIT, 2 ROM */
    if (argc > 1)
        mode = (argv[1][0] | 0x20) == 'i' ? 1 : (argv[1][0] | 0x20) == 'r' ? 2 : 0;
    g_ring0 = (get_cs() & 3) == 0;
    calibrate();
    cpuid4(1, 0, r);
    out("=== SMITIME %s === ring %u, TSC %u MHz, APIC ID %u\n", mode == 1 ? "INIT" : mode == 2 ? "ROM" : "",
        get_cs() & 3, g_per_us, r[1] >> 24);
    g_apic = g_ring0 ? (volatile u32 *) 0xfee00000u : (volatile u32 *) map_phys(0xfee00000u, 4096);
    if (g_apic)
        out("local APIC: ID %08x SVR %08x LVT0 %08x LVT1 %08x TPR %08x ESR %08x\n", g_apic[0x20 / 4],
            g_apic[0xf0 / 4], g_apic[0x350 / 4], g_apic[0x360 / 4], g_apic[0x80 / 4], g_apic[0x280 / 4]);
    measure("before");
    if (mode && !g_apic) {
        out("no APIC mapping, no IPIs\n");
    } else if (mode == 1) {
        ipi_all(0x4500); /* INIT, level assert */
        wait_us(10000);
        out("INIT sent to all other cores\n");
        measure("after INIT");
    } else if (mode == 2) {
        u32      hlt = find_rom_hlt();
        unsigned sel, seg = hlt ? dos_alloc(0x200, &sel) : 0;
        if (!hlt) {
            out("no F4 EB FD in the BIOS ROM\n");
        } else if (!seg) {
            out("no DOS memory\n");
        } else {
            u32 page = ((u32) seg * 16 + 0xfff) & ~0xfffu;
            u8 *c    = (u8 *) page;
            c[0]     = 0xfa; /* cli */
            c[1]     = 0xea; /* jmp far F000:off */
            c[2]     = (u8) (hlt - 0xf0000);
            c[3]     = (u8) ((hlt - 0xf0000) >> 8);
            c[4]     = 0x00;
            c[5]     = 0xf0;
            out("ROM hlt loop at %05x, trampoline %05x\n", hlt, page);
            ipi_all(0x4500);
            wait_us(10000);
            for (int k = 0; k < 2; k++) {
                ipi_all(0x4600 | (page >> 12));
                wait_us(200);
            }
            wait_us(10000);
            dos_free(sel);
            out("cores parked in the ROM loop\n");
            measure("after ROM park");
        }
    }
    out("done\n");
    return 0;
}
