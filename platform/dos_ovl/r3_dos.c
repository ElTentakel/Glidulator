/* Privileged instructions in ring 3 under a DPMI host (Open Watcom).
 *
 * Under HDPMI32i (SBEMU) and VDPMI the OVL runs in ring 3: SBEMU traps the
 * Sound Blaster ports through the I/O permission bitmap, which only works
 * above IOPL. What the host does with a privileged instruction differs:
 * HDPMI emulates reading CR0/CR3/CR4 (real values) and writing CR4, VDPMI
 * passes #GP for CR0/CR4 to the client, returns 0 for CR3 and emulates
 * RDMSR. Each instruction therefore runs behind a DPMI exception handler
 * that skips it on a fault (tools/dosvm/r3probe.c checks the same).
 *
 * The handlers are only installed around the probes (r3_guard_begin/end),
 * never while the game runs. */

#include "r3_dos.h"

static volatile uint32_t g_skip, g_fault;
static uint16_t          g_ds;
static uint32_t          g_old6[2], g_old13[2], g_old14[2];
static int               g_guarded;

/* DPMI 32-bit exception frame after push ds/eax: [esp] eax, [esp+4] ds,
 * [esp+8] return EIP, [esp+12] return CS, [esp+16] error code, [esp+20]
 * faulting EIP. DS is loaded through CS (flat, base 0). Faults outside a
 * probe (g_skip 0) go to the previous handler. */
static void __declspec(naked) r3_exc6(void) {
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
static void __declspec(naked) r3_exc13(void) {
    _asm {
        push ds
        push eax
        mov ax, word ptr cs:g_ds
        mov ds, ax
        cmp g_skip, 0
        je chain13
        mov g_fault, 13
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
static void __declspec(naked) r3_exc14(void) {
    _asm {
        push ds
        push eax
        mov ax, word ptr cs:g_ds
        mov ds, ax
        cmp g_skip, 0
        je chain14
        mov g_fault, 14
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

static unsigned r3_get_cs(void);
#pragma aux r3_get_cs = "xor eax, eax" "mov ax, cs" value [eax];
static unsigned r3_get_ds(void);
#pragma aux r3_get_ds = "xor eax, eax" "mov ax, ds" value [eax];

/* DPMI 0202h: exception n -> CX:EDX */
static void dpmi_get_exc(unsigned n, uint32_t *old);
#pragma aux dpmi_get_exc = "mov ax, 0202h" "int 31h" "mov [esi], edx" "movzx ecx, cx" "mov [esi+4], ecx" \
    parm [ebx] [esi] modify [eax ecx edx];
/* DPMI 0203h: exception n <- CX:EDX; 0 = ok */
static unsigned dpmi_set_exc(unsigned n, unsigned sel, uint32_t off);
#pragma aux dpmi_set_exc = "mov ax, 0203h" "int 31h" "sbb eax, eax" parm [ebx] [ecx] [edx] value [eax];

int r3_guard_begin(void) {
    unsigned cs = r3_get_cs();
    if (g_guarded)
        return 1;
    g_ds = (uint16_t) r3_get_ds();
    dpmi_get_exc(6, g_old6);
    dpmi_get_exc(13, g_old13);
    dpmi_get_exc(14, g_old14);
    if (dpmi_set_exc(6, cs, (uint32_t) r3_exc6) || dpmi_set_exc(13, cs, (uint32_t) r3_exc13) ||
        dpmi_set_exc(14, cs, (uint32_t) r3_exc14)) {
        g_guarded = 1;
        r3_guard_end();
        return 0;
    }
    g_guarded = 1;
    return 1;
}

void r3_guard_end(void) {
    if (!g_guarded)
        return;
    dpmi_set_exc(6, g_old6[1], g_old6[0]);
    dpmi_set_exc(13, g_old13[1], g_old13[0]);
    dpmi_set_exc(14, g_old14[1], g_old14[0]);
    g_guarded = 0;
}

/* The instructions as bytes, EAX cleared first (VDPMI "emulates" CR3 as 0) */
static uint32_t rd_cr0(void);
#pragma aux rd_cr0 = "xor eax, eax" 0x0f 0x20 0xc0 value [eax];
static uint32_t rd_cr3(void);
#pragma aux rd_cr3 = "xor eax, eax" 0x0f 0x20 0xd8 value [eax];
static uint32_t rd_cr4(void);
#pragma aux rd_cr4 = "xor eax, eax" 0x0f 0x20 0xe0 value [eax];
static void rd_msr(uint32_t msr, uint32_t *lo, uint32_t *hi);
#pragma aux rd_msr = "xor eax, eax" "xor edx, edx" 0x0f 0x32 "mov [esi], eax" "mov [edi], edx" \
    parm [ecx] [esi] [edi] modify [eax edx];
static void sse_xorps(void);
#pragma aux sse_xorps = 0x0f 0x57 0xc0; /* xorps xmm0, xmm0 */

#define TRY(len, stmt) (g_fault = 0, g_skip = (len), (stmt), g_skip = 0, g_fault == 0)

int r3_try_read_cr(int n, uint32_t *v) {
    if (!g_guarded)
        return 0;
    switch (n) {
    case 0:
        return TRY(3, *v = rd_cr0());
    case 3:
        return TRY(3, *v = rd_cr3());
    case 4:
        return TRY(3, *v = rd_cr4());
    }
    return 0;
}

int r3_try_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi) {
    return g_guarded && TRY(2, rd_msr(msr, lo, hi));
}

int r3_try_sse(void) {
    return g_guarded && TRY(3, sse_xorps());
}

/* DPMI 0800h: BX:CX physical, SI:DI size -> BX:CX linear */
static void *dpmi_map(uint32_t phys, uint32_t size);
#pragma aux dpmi_map =         \
    "mov ecx, eax"             \
    "shr eax, 16"              \
    "mov ebx, eax"             \
    "mov edi, edx"             \
    "shr edx, 16"              \
    "mov esi, edx"             \
    "mov ax, 0800h"            \
    "int 31h"                  \
    "jc short map_fail"        \
    "shl ebx, 16"              \
    "mov bx, cx"               \
    "mov eax, ebx"             \
    "jmp short map_done"       \
    "map_fail:"                \
    "xor eax, eax"             \
    "map_done:"                \
    parm [eax] [edx] value [eax] modify [ebx ecx edx esi edi];
/* DPMI 0801h (DPMI 1.0; hosts without it keep the mapping) */
static void dpmi_unmap(uint32_t lin);
#pragma aux dpmi_unmap = "mov ecx, eax" "shr eax, 16" "mov ebx, eax" "mov ax, 0801h" "int 31h" \
    parm [eax] modify [eax ebx ecx];
/* DPMI 0006h: CX:DX = base */
static uint32_t dpmi_base(unsigned sel);
#pragma aux dpmi_base =        \
    "mov ax, 0006h"            \
    "int 31h"                  \
    "jc short base_fail"       \
    "shl ecx, 16"              \
    "mov cx, dx"               \
    "mov eax, ecx"             \
    "jmp short base_done"      \
    "base_fail:"               \
    "mov eax, 0ffffffffh"      \
    "base_done:"               \
    parm [ebx] value [eax] modify [ecx edx];
static uint32_t lsl32(unsigned sel);
#pragma aux lsl32 = "xor eax, eax" "lsl eax, edx" parm [edx] value [eax];

void *r3_map_phys(uint32_t phys, uint32_t size) {
    return dpmi_map(phys, size);
}

void r3_unmap(void *lin) {
    if (lin)
        dpmi_unmap((uint32_t) lin);
}

uint32_t r3_sel_base(unsigned sel) {
    return dpmi_base(sel);
}

uint32_t r3_sel_limit(unsigned sel) {
    return lsl32(sel);
}
