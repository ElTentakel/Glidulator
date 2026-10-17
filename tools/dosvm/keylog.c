/* Keyboard IRQ probe for the OVL's Print Screen hook (keyb_dos.c): what
 * does port 60h return to a protected-mode INT 09h handler that chains to
 * the previous one and reads the scan code afterwards? With a USB keyboard
 * the BIOS emulates the 8042 in SMM; a second read may differ from real
 * hardware.
 *
 *   KEYLOG       our handler -> BIOS handler (DOS4GW/host reflection)
 *   KEYLOG G     our handler -> "game" handler (reads 60h itself, EOI,
 *                does not chain), like a DOS game under the OVL
 *   ... N        (KEYLOG N, KEYLOG GN) no reads before chaining, exactly
 *                like the OVL (a read may advance an emulated 8042)
 *
 * Per IRQ: port 64h and 60h before chaining, 64h and 60h after. Runs 15 s (press
 * keys meanwhile, e.g. Print Screen), then appends the log to KEYLOG.TXT.
 * Built by tools/dosvm/run.sh. */
#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <i86.h>

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define NLOG 256

static unsigned          g_old[2];  /* offset, selector of the previous handler */
static unsigned          g_ds;
static volatile unsigned g_n;
static volatile u8       g_st[NLOG], g_pre[NLOG], g_st2[NLOG], g_post[NLOG];
static volatile u8       g_game[NLOG];
static volatile unsigned g_ngame;
static unsigned          g_nopre;

/* our hook, like kb_irq in keyb_dos.c plus the reads before chaining */
static void __declspec(naked) hook(void) {
    _asm {
        push eax
        push ebx
        push ds
        mov ax, word ptr cs:g_ds
        mov ds, ax
        mov ebx, g_n
        and ebx, NLOG - 1
        mov al, 0ffh
        cmp g_nopre, 0
        jne skip_pre
        in al, 64h
        mov byte ptr g_st[ebx], al
        in al, 60h
    skip_pre:
        mov byte ptr g_pre[ebx], al
        pushfd
        call fword ptr g_old
        mov ebx, g_n
        and ebx, NLOG - 1
        in al, 64h
        mov byte ptr g_st2[ebx], al
        in al, 60h
        mov byte ptr g_post[ebx], al
        inc g_n
        pop ds
        pop ebx
        pop eax
        iretd
    }
}

/* a game's handler: read the scan code, acknowledge, EOI, no chaining */
static void __declspec(naked) game(void) {
    _asm {
        push eax
        push ebx
        push ds
        mov ax, word ptr cs:g_ds
        mov ds, ax
        in al, 60h
        mov ebx, g_ngame
        and ebx, NLOG - 1
        mov byte ptr g_game[ebx], al
        inc g_ngame
        in al, 61h
        or al, 80h
        out 61h, al
        and al, 7fh
        out 61h, al
        mov al, 20h
        out 20h, al
        pop ds
        pop ebx
        pop eax
        iretd
    }
}

static unsigned get_cs(void);
#pragma aux get_cs = "xor eax, eax" "mov ax, cs" value [eax];
static unsigned get_ds(void);
#pragma aux get_ds = "xor eax, eax" "mov ax, ds" value [eax];

static void get_vec(unsigned vec, unsigned *off, unsigned *sel) {
    union REGS r;
    memset(&r, 0, sizeof r);
    r.w.ax = 0x0204;
    r.h.bl = (u8) vec;
    int386(0x31, &r, &r);
    *off = r.x.edx;
    *sel = r.w.cx;
}

static int set_vec(unsigned vec, unsigned sel, unsigned off) {
    union REGS r;
    memset(&r, 0, sizeof r);
    r.w.ax  = 0x0205;
    r.h.bl  = (u8) vec;
    r.w.cx  = (u16) sel;
    r.x.edx = off;
    int386(0x31, &r, &r);
    return !r.w.cflag;
}

int main(int argc, char **argv) {
    int      gmode = argc > 1 && (argv[1][0] | 0x20) == 'g';
    g_nopre        = argc > 1 && (strchr(argv[1], 'n') || strchr(argv[1], 'N'));
    unsigned orig_off, orig_sel;
    FILE    *f;
    g_ds = get_ds();
    get_vec(9, &orig_off, &orig_sel);
    if (gmode)
        set_vec(9, get_cs(), (unsigned) game);
    get_vec(9, &g_old[0], &g_old[1]);
    if (!set_vec(9, get_cs(), (unsigned) hook)) {
        printf("INT 09h not hookable\n");
        return 1;
    }
    printf("KEYLOG %s: CPL %u, 15 s, press keys (Print Screen)...\n", gmode ? "G" : "", get_cs() & 3);
    {
        volatile u32 *tick = (volatile u32 *) 0x46c;
        u32           t0   = *tick;
        while (*tick - t0 < 273) {
            if (!gmode) /* empty the BIOS buffer: head = tail */
                *(volatile u16 *) 0x41a = *(volatile u16 *) 0x41c;
        }
    }
    set_vec(9, orig_sel, orig_off);
    f = fopen("KEYLOG.TXT", "a");
    fprintf(f, "=== KEYLOG %s%s === CPL %u, %u IRQs, game handler %u bytes\n", gmode ? "G" : "", g_nopre ? "N" : "", get_cs() & 3,
            g_n, g_ngame);
    fprintf(f, "  (before chaining: 64h, 60h; after: 64h, 60h%s)\n", gmode ? ", 60h read by the game" : "");
    for (unsigned i = 0; i < g_n && i < NLOG; i++) {
        fprintf(f, "  %02x %02x  %02x %02x", g_st[i], g_pre[i], g_st2[i], g_post[i]);
        if (gmode && i < g_ngame)
            fprintf(f, " %02x", g_game[i]);
        fprintf(f, "\n");
    }
    fclose(f);
    printf("%u IRQs, written to KEYLOG.TXT\n", g_n);
    return 0;
}
