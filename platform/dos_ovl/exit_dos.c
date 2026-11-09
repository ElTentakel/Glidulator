/* Clean-up when a game ends while the Glide display is still open (Open
 * Watcom).
 *
 * A game that exits without grSstWinClose (for example Dimension when its
 * sound card is missing: MIDAS gives up after grSstWinOpen) left the
 * render cores running on freed memory, our INT 09h handler in freed
 * memory (keyboard dead, Ctrl+Alt+Del too), write-combining on and the
 * video mode set. So while the display is open, the OVL hooks the
 * protected-mode INT 21h (DPMI 0204h/0205h): on AH=4Ch it switches to a
 * stack of its own, runs the same tear-down as grSstWinClose (render
 * workers and cores stopped, INT 09h, WC/PAT, CR4 and text mode back) and
 * passes the call on. Every other INT 21h function goes straight to the
 * previous handler.
 *
 * Removed at grSstWinClose if the vector still points to us; if the game
 * has hooked INT 21h in front of us meanwhile, our handler stays in its
 * chain and only passes calls on. Not under tools/leload_demo (Linux). */

#include "glidulator/platform.h"

#include "host_memory.h"

#define EXIT_STACK_BYTES 32768

static unsigned          g_ex_old[2]; /* offset, selector: fword for the jump */
static unsigned          g_ex_ds;
static volatile unsigned g_ex_active; /* display open, clean-up wanted */
static volatile unsigned g_ex_busy;   /* clean-up running (its own INT 21h calls) */
static unsigned          g_ex_sv[2];  /* game's ESP, SS */
static unsigned          g_ex_top;    /* top of our stack */
static int               g_ex_hooked;
static void (*g_ex_fn)(void);
static uint8_t g_ex_stack[EXIT_STACK_BYTES];

static void exit_cleanup(void) {
    if (g_ex_active && g_ex_fn) {
        g_ex_active = 0;
        g_ex_fn();
    }
}

static void __declspec(naked) ex_int21(void) {
    _asm {
        cmp ah, 4Ch
        jne chain
        cmp dword ptr cs:g_ex_active, 0
        je chain
        cmp dword ptr cs:g_ex_busy, 0
        jne chain
        pushad
        push ds
        push es
        mov ax, word ptr cs:g_ex_ds
        mov ds, ax
        mov es, ax
        mov g_ex_busy, 1
        mov dword ptr g_ex_sv, esp
        mov word ptr g_ex_sv[4], ss
        mov ss, ax
        mov esp, g_ex_top
        sti
        call exit_cleanup
        cli
        lss esp, fword ptr g_ex_sv
        mov g_ex_busy, 0
        pop es
        pop ds
        popad
    chain:
        jmp fword ptr cs:g_ex_old
    }
}

static unsigned ex_get_cs(void);
#pragma aux ex_get_cs = "xor eax, eax" "mov ax, cs" value [eax];
static unsigned ex_get_ds(void);
#pragma aux ex_get_ds = "xor eax, eax" "mov ax, ds" value [eax];
/* DPMI 0204h: protected-mode interrupt vector -> *off, *sel */
static void ex_get_vec(unsigned vec, unsigned *off, unsigned *sel);
#pragma aux ex_get_vec = "mov ax, 0204h" "int 31h" "mov [esi], edx" "movzx ecx, cx" "mov [edi], ecx" \
    parm [ebx] [esi] [edi] modify [eax ecx edx];
/* DPMI 0205h: set it; returns 0 on success */
static unsigned ex_set_vec(unsigned vec, unsigned sel, unsigned off);
#pragma aux ex_set_vec = "mov ax, 0205h" "int 31h" "sbb eax, eax" parm [ebx] [ecx] [edx] value [eax];

void exit_dos_hook(void (*cleanup)(void)) {
    g_ex_fn = cleanup;
    if (platform_host_memory_set()) /* tools/leload_demo on Linux: no DPMI */
        return;
    if (!g_ex_hooked) {
        g_ex_ds  = ex_get_ds();
        g_ex_top = ((unsigned) (g_ex_stack + EXIT_STACK_BYTES)) & ~15u;
        ex_get_vec(0x21, &g_ex_old[0], &g_ex_old[1]);
        g_ex_hooked = ex_set_vec(0x21, ex_get_cs(), (unsigned) ex_int21) == 0;
#ifdef GLIDULATOR_TRACE
        platform_log(g_ex_hooked ? "Exit guard: INT 21h hooked\n" : "Exit guard: INT 21h not hookable\n");
#endif
    }
    g_ex_active = g_ex_hooked;
}

void exit_dos_unhook(void) {
    unsigned off, sel;
    g_ex_active = 0;
    if (!g_ex_hooked)
        return;
    ex_get_vec(0x21, &off, &sel);
    if (off == (unsigned) ex_int21 && sel == ex_get_cs()) {
        ex_set_vec(0x21, g_ex_old[1], g_ex_old[0]);
        g_ex_hooked = 0;
    }
}
