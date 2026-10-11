#ifndef GLIDULATOR_R3_DOS_H
#define GLIDULATOR_R3_DOS_H

/* The OVL in ring 3 under a DPMI host (HDPMI32i for SBEMU, VDPMI):
 * privileged instructions behind a DPMI exception handler, physical
 * mappings and selector bases (r3_dos.c). Unused in ring 0. */

#include <stdint.h>

/* Install/remove the handlers for #UD, #GP and #PF (DPMI 0202h/0203h).
 * Between the two, the r3_try_* functions skip a faulting instruction
 * instead of terminating the program. r3_guard_begin returns 0 if the
 * host refuses. */
int  r3_guard_begin(void);
void r3_guard_end(void);

/* mov eax, crN (N = 0, 3, 4): 1 and *v on success, 0 on a fault. Some
 * hosts emulate the read (HDPMI), others return 0 (VDPMI: CR3). */
int r3_try_read_cr(int n, uint32_t *v);
int r3_try_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi);
/* xorps: SSE usable (CR4.OSFXSR set by the host)? */
int r3_try_sse(void);

/* DPMI 0800h/0801h: map physical memory; NULL on failure */
void *r3_map_phys(uint32_t phys, uint32_t size);
void  r3_unmap(void *lin);
/* DPMI 0006h: base of a selector (0xffffffff on failure); LSL limit */
uint32_t r3_sel_base(unsigned sel);
uint32_t r3_sel_limit(unsigned sel);

#endif
