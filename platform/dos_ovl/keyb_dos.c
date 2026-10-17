/* Screenshot key for the OVL: Print Screen (platform_screenshot_key,
 * core/screenshot.c).
 *
 * The BIOS turns Print Screen into INT 05h, but DOS games install their
 * own keyboard handler and rarely call the BIOS's. So while the display is
 * open, the OVL puts a protected-mode handler for IRQ 1 (INT 09h, DPMI
 * 0204h/0205h) in front of the game's: grSstWinOpen comes after the game
 * has installed its handler. Our handler first calls the previous one, so
 * the game reads port 60h and its status as without us, then looks at the
 * last scan code (it stays in port 60h) for Print Screen's make code
 * E0 37 and counts it once per press. Some handlers read several bytes per
 * IRQ, so we see only the last of each burst (on release often E0 AA
 * instead of E0 B7): every byte other than E0 and 37 ends the press, key
 * repeat (E0 37 E0 37 ...) does not. Nothing else changes: no EOI, no PIC
 * access.
 *
 * Removed at grSstWinClose if the vector still points to us. If the game
 * has hooked in front of us meanwhile, our handler stays in its chain; it
 * only reads port 60h and passes on, so that is harmless.
 * GLIDULATOR_SCREENSHOT=0 turns the hook off. */

#include "glidulator/platform.h"

#include "host_memory.h"

static unsigned          g_kb_old[2]; /* offset, selector: fword for the far call */
static unsigned          g_kb_ds;
static volatile unsigned g_kb_count; /* Print Screen presses */
static unsigned          g_kb_taken; /* presses already reported */
static unsigned          g_kb_e0, g_kb_down;
static int               g_kb_hooked;
static volatile unsigned g_kb_irqs;     /* for the Trace log: IRQs seen ... */
static volatile uint8_t  g_kb_seen[64]; /* ... and the last scan codes */

static void __declspec(naked) kb_irq(void) {
    _asm {
        push eax
        push ebx
        push ds
        mov ax, word ptr cs:g_kb_ds
        mov ds, ax
        pushfd
        call fword ptr g_kb_old
        in al, 60h
        mov ebx, g_kb_irqs
        and ebx, 63
        mov byte ptr g_kb_seen[ebx], al
        inc g_kb_irqs
        cmp al, 0E0h
        jne not_e0
        mov g_kb_e0, 1
        jmp done
    not_e0:
        cmp al, 37h
        je no_release
        mov g_kb_down, 0
    no_release:
        cmp g_kb_e0, 0
        mov g_kb_e0, 0
        je done
        cmp al, 37h
        jne done
        cmp g_kb_down, 0
        jne done
        mov g_kb_down, 1
        inc g_kb_count
    done:
        pop ds
        pop ebx
        pop eax
        iretd
    }
}

static unsigned kb_get_cs(void);
#pragma aux kb_get_cs = "xor eax, eax" "mov ax, cs" value [eax];
static unsigned kb_get_ds(void);
#pragma aux kb_get_ds = "xor eax, eax" "mov ax, ds" value [eax];
/* DPMI 0204h: protected-mode interrupt vector -> *off, *sel */
static void kb_get_vec(unsigned vec, unsigned *off, unsigned *sel);
#pragma aux kb_get_vec = "mov ax, 0204h" "int 31h" "mov [esi], edx" "movzx ecx, cx" "mov [edi], ecx" \
    parm [ebx] [esi] [edi] modify [eax ecx edx];
/* DPMI 0205h: set it; returns 0 on success */
static unsigned kb_set_vec(unsigned vec, unsigned sel, unsigned off);
#pragma aux kb_set_vec = "mov ax, 0205h" "int 31h" "sbb eax, eax" parm [ebx] [ecx] [edx] value [eax];

void keyb_dos_hook(void) {
    char cfg[4];
    if (g_kb_hooked || platform_host_memory_set()) /* tools/leload_demo on Linux: no DPMI */
        return;
    if (platform_get_config("GLIDULATOR_SCREENSHOT", cfg, sizeof cfg) && cfg[0] == '0')
        return;
    g_kb_ds = kb_get_ds();
    kb_get_vec(9, &g_kb_old[0], &g_kb_old[1]);
    g_kb_hooked = kb_set_vec(9, kb_get_cs(), (unsigned) kb_irq) == 0;
#ifdef GLIDULATOR_TRACE
    platform_log(g_kb_hooked ? "Screenshot: Print Screen hooked (INT 09h)\n" : "Screenshot: INT 09h not hookable\n");
#endif
}

void keyb_dos_unhook(void) {
    unsigned off, sel;
    if (!g_kb_hooked)
        return;
#ifdef GLIDULATOR_TRACE
    {
        static const char hex[] = "0123456789abcdef";
        char              line[64 * 3 + 80], *p = line;
        unsigned          n = g_kb_irqs, first = n > 64 ? n - 64 : 0;
        const char       *h = "Screenshot: IRQs ";
        while (*h)
            *p++ = *h++;
        for (unsigned v = n, d = 1000000; d; d /= 10)
            if (v >= d || d == 1)
                *p++ = (char) ('0' + v / d % 10);
        h = ", presses ";
        while (*h)
            *p++ = *h++;
        *p++ = (char) ('0' + g_kb_count % 10);
        *p++ = ',';
        for (unsigned i = first; i < n; i++) {
            *p++ = ' ';
            *p++ = hex[g_kb_seen[i & 63] >> 4];
            *p++ = hex[g_kb_seen[i & 63] & 15];
        }
        *p++ = '\n';
        *p   = 0;
        platform_log(line);
    }
#endif
    kb_get_vec(9, &off, &sel);
    if (off == (unsigned) kb_irq && sel == kb_get_cs())
        kb_set_vec(9, g_kb_old[1], g_kb_old[0]);
    g_kb_hooked = 0;
}

int platform_screenshot_key(void) {
    unsigned c = g_kb_count;
    if (c == g_kb_taken)
        return 0;
    g_kb_taken = c;
    return 1;
}
