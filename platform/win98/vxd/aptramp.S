/* AP startup trampoline for GLSMP.VXD (glsmp.asm).
 *
 * glsmp.vxd copies this page to a physical page below 1 MB and fills in
 * the parameters below; the Startup IPI starts every AP in real mode at
 * CS:IP = page:0000. The AP loads Windows' GDT (selector loads only happen
 * after paging is on), the VxD's IDT, the main core's CR4 and CR0 and the
 * VxD's own page directory (0-4 MB identity mapped, so the instruction after
 * "mov cr0" is still found; the rest copied from the calling process), then
 * far-jumps to ap_pm32 in the VxD (system arena). DX carries the data selector across
 * the switch to protected mode, since selector loads work only once paging is
 * on; ap_pm32 loads DS/ES/SS from it. Assembled to a flat binary and included
 * into glsmp.asm as bytes (Makefile, target vxd). */

    .set P_GDTR,  0x80   /* 2 bytes limit, 4 bytes base (linear) */
    .set P_IDTR,  0x88
    .set P_CR4,   0x90
    .set P_CR3,   0x94   /* the VxD's page directory (physical) */
    .set P_CR0,   0x98
    .set P_JMP,   0x9c   /* 4 bytes offset (ap_pm32), 2 bytes selector */
    .set P_DSEL,  0xa2   /* data selector (DS/ES/SS) for ap_pm32 */
    .set P_ALIVE, 0xa4   /* counts APs that reached the trampoline */

    .text
    .code16
start:
    cli
    cld
    movw    %cs, %ax
    movw    %ax, %ds
    lock incl P_ALIVE
    movw    P_DSEL, %dx       /* carried through "mov cr0" into ap_pm32 */
    lgdtl   P_GDTR
    lidtl   P_IDTR
    movl    P_CR4, %eax
    movl    %eax, %cr4
    movl    P_CR3, %eax
    movl    %eax, %cr3
    movl    P_CR0, %eax
    movl    %eax, %cr0
    ljmpl   *P_JMP

    .org    0xaf
    .byte   0
