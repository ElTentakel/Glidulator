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

/* Ring 0 for MSR accesses through a temporary call gate in the host's GDT
 * (r3_dos.c). r3_gate_open: needs the host's CR3 (paging, no PAE) and two
 * free GDT entries; maps them once. The MSR calls need no DPMI service
 * and leave nothing in the GDT. 1 = ok. */
int r3_gate_open(uint32_t cr3);
int r3_gate_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi);
int r3_gate_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi);

/* WRMSR through the gate, then a TLB flush (mov cr3), in one ring-0
 * call. 1 = ok. */
int r3_gate_wrmsr_flush(uint32_t msr, uint32_t lo, uint32_t hi);

/* The host's 4 KB page table entries of [lin, lin + size): if every one
 * is present with (e & mask) == from, set them to (e & ~mask) | to and
 * return 1; otherwise change nothing and return 0. Uses DPMI 0800h. The
 * caller flushes the TLB. */
int r3_pte_change(uint32_t cr3, uint32_t lin, uint32_t size, uint32_t mask, uint32_t from, uint32_t to);

/* Page table entry (PTE, or PDE of a 4 MB page: *large = 1) that maps lin
 * in the host's tables; 0 if not reachable. */
uint32_t r3_page_entry(uint32_t cr3, uint32_t lin, int *large);

#endif
