/* vid_bios for real DOS: DPMI services of the DOS extender (DOS/4GW etc.).
 * Open Watcom only; the calls are #pragma aux inline assembly so nothing
 * depends on clib3s.lib's never-initialised CRT.
 *
 * Assumption: flat model with segment base 0 (DOS/4G, PMODE/W, ...), i.e.
 * the first 1 MB is identity-mapped and linear == usable addresses. */

#include "video.h"

/* wc_dos.c */
int VID_CALL  wc_dos_on(uint32_t phys, uint32_t need, uint32_t vram);
void VID_CALL wc_dos_off(void);

/* INT 31h/0300h: simulate a real-mode interrupt, ES:EDI -> register block.
 * Returns 0 on success, -1 with carry set. */
static int dpmi_sim_int(vid_rmregs *r, uint32_t intno);
#pragma aux dpmi_sim_int = \
    "push es"              \
    "push ds"              \
    "pop es"               \
    "mov ax, 0300h"        \
    "xor bh, bh"           \
    "xor ecx, ecx"         \
    "int 31h"              \
    "sbb eax, eax"         \
    "pop es"               \
    parm [edi] [ebx] value [eax] modify [ebx ecx edx esi];

/* INT 31h/0100h: allocate DOS memory (below 1 MB), BX = paragraphs.
 * Returns the real-mode segment, 0 on failure. */
static uint32_t dpmi_dos_alloc(uint32_t paragraphs);
#pragma aux dpmi_dos_alloc = \
    "mov ax, 0100h"          \
    "int 31h"                \
    "sbb ecx, ecx"           \
    "not ecx"                \
    "movzx eax, ax"          \
    "and eax, ecx"           \
    parm [ebx] value [eax] modify [ecx edx];

/* INT 31h/0800h: map a physical address, BX:CX = address, SI:DI = size
 * -> BX:CX = linear address. Returns 0 on failure. */
static uint32_t dpmi_map_phys(uint32_t phys, uint32_t size);
#pragma aux dpmi_map_phys = \
    "mov ecx, eax"          \
    "mov ebx, eax"          \
    "shr ebx, 16"           \
    "mov edi, edx"          \
    "mov esi, edx"          \
    "shr esi, 16"           \
    "mov ax, 0800h"         \
    "int 31h"               \
    "sbb eax, eax"          \
    "not eax"               \
    "shl ebx, 16"           \
    "mov bx, cx"            \
    "and eax, ebx"          \
    parm [eax] [edx] value [eax] modify [ebx ecx edx esi edi];

static void port_out(uint32_t port, uint32_t value);
#pragma aux port_out = \
    "out dx, al"       \
    parm [edx] [eax];

static void VID_CALL dpmi_int10(vid_rmregs *r) {
    /* No own real-mode stack: SS:SP = 0 makes the DPMI host provide one.
     * Flags 0 so no stray trap/interrupt flag gets through. */
    r->flags = 0;
    r->ss = r->sp = 0;
    if (dpmi_sim_int(r, 0x10) != 0)
        r->eax = 0xFFFF; /* neither 004Fh nor a valid mode */
}

static void *VID_CALL flat_real_ptr(uint16_t seg, uint16_t off) {
    return (void *) (((uint32_t) seg << 4) + off);
}

static void *VID_CALL dpmi_map(uint32_t phys, uint32_t size) {
    return (void *) dpmi_map_phys(phys, size);
}

static void VID_CALL dpmi_outb(uint16_t port, uint8_t value) {
    port_out(port, value);
}

static vid_bios g_dpmi_bios;

/* NULL if no DOS memory for the VBE structures is available; output then
 * simply stays off (see vid_set_bios()). The 1 KB buffer is never freed:
 * once per loaded OVL. */
const vid_bios *vid_dpmi_bios(void) {
    if (!g_dpmi_bios.int10) {
        uint32_t seg = dpmi_dos_alloc((VID_DOSBUF_BYTES + 15) / 16);
        if (!seg)
            return NULL;
        g_dpmi_bios.int10      = dpmi_int10;
        g_dpmi_bios.real_ptr   = flat_real_ptr;
        g_dpmi_bios.map_phys   = dpmi_map;
        g_dpmi_bios.outb       = dpmi_outb;
        g_dpmi_bios.dosbuf_seg = (uint16_t) seg;
        g_dpmi_bios.wc_on      = wc_dos_on;
        g_dpmi_bios.wc_off     = wc_dos_off;
    }
    return &g_dpmi_bios;
}
