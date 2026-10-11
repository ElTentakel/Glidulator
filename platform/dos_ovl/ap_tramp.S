/* Startup code for additional CPU cores (APs) of the DOS OVL, see smp_dos.c.
 *
 * Built into build/.../ap_tramp.h (GNU as, objcopy, xxd) and copied by
 * smp_dos.c to a 4 KB boundary in DOS memory (below 1 MB); the Startup IPI
 * starts every AP in real mode at CS:IP = page:0000. Position-independent:
 * the real-mode part addresses relative to CS, the 32-bit part through EBX
 * = linear base. smp_dos.c fills in the parameters at P_BASE beforehand
 * (GDT/IDT, selectors and CR0/CR4 as on the main core, jump target, stacks,
 * entry). The AP uses DOS/4GW's own GDT, so its segment registers equal
 * the main core's (flat, base 0) and C and JIT code run unchanged.
 *
 * Ring 3 under a DPMI host (smp_dos.c): the parameters bring an own flat
 * ring-0 GDT (at TR_GDT in this page) and the host's CR3 (paging host) or
 * none (host without paging). Before the AP touches memory outside this
 * page, the guard compares a heap word and the first 8 code bytes at
 * P_ENTRY with what the main core wrote (G_*): with a wrong address space
 * the AP would otherwise write its stack into foreign memory. A mismatch
 * counts in G_BAD and halts the AP.
 *
 * Then: number via lock xadd, own stack, initialise the FPU, call
 * ap_entry(number) (cdecl, never returns). Cores beyond the maximum and
 * exceptions (IDT below, 32 entries) end in cli/hlt; an exception is first
 * recorded in P_EXC_* (smp_dos.c recognises the failed AP by P_EXC_ESP; with
 * several, the last one is there and P_EXC_FLAG counts all). Afterwards
 * only INIT wakes the AP again (smp_dos.c, stop_aps). */

    .set P_BASE,      0x100
    .set P_GDTR,      P_BASE + 0   /* 2 bytes limit, 4 bytes base */
    .set P_IDTR,      P_BASE + 8
    .set P_JMP,       P_BASE + 16  /* 4 bytes target (linear), 2 bytes selector */
    .set P_DSEL,      P_BASE + 24  /* data segment (DS/ES/SS/FS/GS) */
    .set P_CR0,       P_BASE + 28
    .set P_CR4,       P_BASE + 32
    .set P_COUNT,     P_BASE + 36  /* started APs (lock xadd) */
    .set P_MAX,       P_BASE + 40  /* only this many call ap_entry */
    .set P_STACK,     P_BASE + 44  /* stacks: base ... */
    .set P_STSIZE,    P_BASE + 48  /* ... and size per AP */
    .set P_ENTRY,     P_BASE + 52
    .set P_EXC_FLAG,  P_BASE + 56
    .set P_EXC_VEC,   P_BASE + 60
    .set P_EXC_W0,    P_BASE + 64  /* the three stack words after the vector: */
    .set P_EXC_W1,    P_BASE + 68  /* EIP, CS, EFLAGS or error code, EIP, CS */
    .set P_EXC_W2,    P_BASE + 72
    .set P_EXC_ESP,   P_BASE + 76  /* stack at the fault -> which AP */
    .set P_MC_N,      P_BASE + 80  /* machine check (vector 18): banks ... */
    .set P_MC,        P_BASE + 84  /* ... STATUS (8) and ADDR (8) each, max. 10 */
    .set P_CR3,       P_BASE + 244 /* page directory (paging hosts; else unused) */
    .set G_HEAP,      0x680        /* guard: address of a heap word ... */
    .set G_MAGIC,     0x684        /* ... and its value */
    .set G_CODE0,     0x688        /* first 8 bytes at P_ENTRY */
    .set G_CODE1,     0x68c
    .set G_BAD,       0x690        /* APs stopped by the guard */
    .set P_APREC,     0x500        /* 48 bytes per AP: vector, EIP, EDI ESI EBP ESP
                                      EBX EDX ECX EAX (pushal), valid; 8 APs */

    .text
    .code16
start:
    cli
    cld
    xorl    %ebx, %ebx
    movw    %cs, %bx
    shll    $4, %ebx
    movw    %cs, %ax
    movw    %ax, %ds
    lgdtl   P_GDTR
    lidtl   P_IDTR
    movw    P_DSEL, %dx
    movl    P_CR4, %eax
    movl    %eax, %cr4
    movl    P_CR3, %eax
    movl    %eax, %cr3
    movl    P_CR0, %eax
    movl    %eax, %cr0
    ljmpl   *P_JMP

    .org    0x60
    .code32
pm32:
    movw    %dx, %ds
    movw    %dx, %es
    movw    %dx, %fs
    movw    %dx, %gs
    movw    %dx, %ss
    xorl    %eax, %eax
    lldt    %ax
    movl    G_HEAP(%ebx), %esi
    movl    (%esi), %eax
    cmpl    G_MAGIC(%ebx), %eax
    jne     bad
    movl    P_ENTRY(%ebx), %esi
    movl    (%esi), %eax
    cmpl    G_CODE0(%ebx), %eax
    jne     bad
    movl    4(%esi), %eax
    cmpl    G_CODE1(%ebx), %eax
    jne     bad
    movl    $1, %eax
    lock xaddl %eax, P_COUNT(%ebx)
    cmpl    P_MAX(%ebx), %eax
    jae     park
    leal    1(%eax), %ecx
    imull   P_STSIZE(%ebx), %ecx
    addl    P_STACK(%ebx), %ecx
    movl    %ecx, %esp
    fninit
    pushl   %eax
    calll   *P_ENTRY(%ebx)
park:
    cli
    hlt
    jmp     park
bad:
    lock incl G_BAD(%ebx)
    jmp     park

    /* exception entries, 8 bytes each from 0x200 (smp_dos.c builds the IDT) */
    .irp v, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    .org    0x200 + 8 * \v
    pushl   $\v
    jmp     exc_common
    .endr

    .org    0x300
exc_common:
    pushal                         /* registers of this AP into its record */
    call    1f
1:  popl    %ebp
    subl    $(1b - start), %ebp
    leal    32(%esp), %eax         /* stack at the fault -> AP number */
    subl    P_STACK(%ebp), %eax
    xorl    %edx, %edx
    divl    P_STSIZE(%ebp)
    cmpl    $8, %eax
    jae     7f
    imull   $48, %eax, %edi
    leal    P_APREC(%ebp,%edi), %edi
    movl    32(%esp), %eax         /* vector */
    movl    %eax, 0(%edi)
    movl    36(%esp), %eax         /* EIP */
    movl    %eax, 4(%edi)
    xorl    %ecx, %ecx
8:  movl    (%esp,%ecx,4), %eax
    movl    %eax, 8(%edi,%ecx,4)
    incl    %ecx
    cmpl    $8, %ecx
    jb      8b
    movl    $1, 40(%edi)
7:  popal
    call    1f
1:  popl    %ebp
    subl    $(1b - start), %ebp
    popl    %eax
    movl    %eax, P_EXC_VEC(%ebp)
    movl    0(%esp), %ecx
    movl    %ecx, P_EXC_W0(%ebp)
    movl    4(%esp), %ecx
    movl    %ecx, P_EXC_W1(%ebp)
    movl    8(%esp), %ecx
    movl    %ecx, P_EXC_W2(%ebp)
    movl    %esp, P_EXC_ESP(%ebp)
    cmpl    $18, %eax              /* machine check: save this core's banks */
    jne     3f
    movl    $0x179, %ecx           /* MCG_CAP */
    rdmsr
    movzbl  %al, %esi
    cmpl    $10, %esi
    jbe     4f
    movl    $10, %esi
4:  movl    %esi, P_MC_N(%ebp)
    xorl    %edi, %edi
5:  cmpl    %esi, %edi
    jae     3f
    leal    0x401(,%edi,4), %ecx   /* MCi_STATUS */
    rdmsr
    movl    %edi, %ebx
    shll    $4, %ebx
    movl    %eax, P_MC(%ebp,%ebx)
    movl    %edx, P_MC+4(%ebp,%ebx)
    incl    %ecx                   /* MCi_ADDR (valid only with ADDRV) */
    btl     $26, %edx
    jnc     6f
    rdmsr
    movl    %eax, P_MC+8(%ebp,%ebx)
    movl    %edx, P_MC+12(%ebp,%ebx)
6:  incl    %edi
    jmp     5b
3:  lock incl P_EXC_FLAG(%ebp)
2:  cli
    hlt
    jmp     2b
