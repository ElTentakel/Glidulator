/* platform.h implementation for GLIDE2X.OVL (Open Watcom, DOS/4G LE DLL).
 * Separate from platform/dos (DJGPP, standalone .exe test programs).
 *
 * platform_alloc() is NOT malloc(): the DLL has no CRT startup ("option
 * nodefaultlibs"), and clib3s.lib's malloc() depends on heap
 * initialisation done by exactly that startup; without it, malloc()
 * dereferences an unset internal heap pointer. Instead every block comes
 * straight from the extender via DPMI (INT 31h, AX=0501h) and is returned
 * with AX=0502h.
 *
 * Memory is not reserved statically: the DLL loader that the 3dfx SDK
 * links into programs allocates every object completely at load time and
 * fails ("unable to load DLL") on large BSS objects. The OVL therefore
 * occupies almost nothing at load time; the large buffers (texture cache,
 * TMU memory, framebuffer) are allocated in grSstWinOpen().
 *
 * tools/leload_demo (Linux) has no INT 31h: there the loader installs the
 * host's malloc/free via glidulatorDebugSetMemory() (see
 * targets/glide_ovl/dllmain.c).
 *
 * Additional CPU cores (platform_core_count, platform_spawn_worker):
 * smp_dos.c. */

#include "glidulator/config.h"
#include "glidulator/platform.h"

#include "host_memory.h"
#include "r3_dos.h"

/* DPMI 0501h: allocate a block of size bytes; BX:CX = linear address
 * (directly usable as a pointer with the zero-based DS of DOS/4GW and
 * DOS/32A), SI:DI = handle for 0502h. NULL on failure. */
static void *dpmi_alloc(uint32_t size, uint32_t *handle);
#pragma aux dpmi_alloc =  \
    "push edx"             \
    "mov ecx, eax"         \
    "shr eax, 16"          \
    "mov ebx, eax"         \
    "mov ax, 0501h"        \
    "int 31h"              \
    "pop edx"              \
    "jc short fail"        \
    "mov [edx], di"        \
    "mov [edx+2], si"      \
    "mov eax, ebx"         \
    "shl eax, 16"          \
    "mov ax, cx"           \
    "jmp short done"       \
    "fail:"                \
    "xor eax, eax"         \
    "done:"                \
    parm [eax] [edx] value [eax] modify [ebx ecx esi edi];

static void dpmi_free(uint32_t handle);
#pragma aux dpmi_free = \
    "mov edi, eax"       \
    "shr eax, 16"        \
    "mov esi, eax"       \
    "mov ax, 0502h"      \
    "int 31h"            \
    parm [eax] modify [eax esi edi];

static platform_host_alloc_fn g_host_alloc;
static platform_host_free_fn  g_host_free;

void platform_set_host_memory(platform_host_alloc_fn alloc, platform_host_free_fn free_fn) {
    g_host_alloc = alloc;
    g_host_free  = free_fn;
}

int platform_host_memory_set(void) {
    return g_host_alloc != NULL;
}

/* A 16-byte header with the DPMI handle precedes every block (keeps the
 * payload 16-byte aligned; DPMI blocks start at page boundaries). */
#define BLOCK_HEADER 16u

/* CPUID exists only from late 486s on: first check whether the EFLAGS ID
 * bit (21) can be toggled (returns the toggleable bits). */
static unsigned eflags_id_toggles(void);
#pragma aux eflags_id_toggles = \
    "pushfd"                  \
    "pop eax"                 \
    "mov ecx, eax"            \
    "xor eax, 200000h"        \
    "push eax"                \
    "popfd"                   \
    "pushfd"                  \
    "pop eax"                 \
    "push ecx"                \
    "popfd"                   \
    "xor eax, ecx"            \
    value [eax] modify [ecx];

/* CPUID function 1, EDX (feature bits). Emitted as bytes because the
 * inline assembler rejects the 586 instruction with -3s. */
static unsigned cpuid1_edx(void);
#pragma aux cpuid1_edx = \
    "mov eax, 1"        \
    0x0f 0xa2           \
    value [edx] modify [eax ebx ecx];

/* SSE/SSE2 (for the JIT) need CR4.OSFXSR. In ring 0 (DOS/4GW without
 * DPMI host) the OVL sets it if the BIOS did not, and
 * platform_cpu_release() (at grSstWinClose) clears it again. In ring 3
 * under a DPMI host one SSE instruction behind an exception handler shows
 * whether the host set it (HDPMI and Windows 98SE do, VDPMI does not). Without the bit every SSE instruction raises #UD. MMX only needs
 * the FPU registers and always works. */
static unsigned ovl_get_cs(void);
#pragma aux ovl_get_cs = "mov ax, cs" "movzx eax, ax" value [eax];
static unsigned ovl_get_cr4(void);
#pragma aux ovl_get_cr4 = 0x0f 0x20 0xe0 value [eax]; /* mov eax, cr4 */
static void ovl_set_cr4(unsigned v);
#pragma aux ovl_set_cr4 = 0x0f 0x22 0xe0 parm [eax]; /* mov cr4, eax */

#define CR4_OSFXSR 0x200u
static int      g_cr4_changed;
static unsigned g_cr4_orig;

static int g_r3_sse = -1; /* ring 3: -1 untested, 0/1 */

unsigned platform_cpu_features(void) {
    if (!(eflags_id_toggles() & 0x200000u))
        return 0;
    unsigned edx = cpuid1_edx();
    unsigned f   = (edx & (1u << 23)) ? PLATFORM_CPU_MMX : 0;
    if ((ovl_get_cs() & 3) && (edx & (1u << 24)) && (edx & (1u << 26)) && !platform_host_memory_set()) {
        if (g_r3_sse < 0) {
            g_r3_sse = r3_guard_begin() && r3_try_sse();
            r3_guard_end();
        }
        if (g_r3_sse)
            f |= PLATFORM_CPU_SSE | PLATFORM_CPU_SSE2;
    }
    /* FXSR (24) and SSE2 (26); ring 0 */
    if ((ovl_get_cs() & 3) == 0 && (edx & (1u << 24)) && (edx & (1u << 26))) {
        unsigned cr4 = ovl_get_cr4();
        if (!(cr4 & CR4_OSFXSR)) {
            g_cr4_orig    = cr4;
            g_cr4_changed = 1;
            ovl_set_cr4(cr4 | CR4_OSFXSR);
        }
        f |= PLATFORM_CPU_SSE | PLATFORM_CPU_SSE2;
    }
    return f;
}

/* Counterpart: leave CR4 as it was (targets/glide_ovl). */
void platform_cpu_release(void) {
    if (g_cr4_changed) {
        ovl_set_cr4(g_cr4_orig);
        g_cr4_changed = 0;
    }
}

/* Reserve after every block. DOS/4GW (without memory manager) hands out
 * memory from the top, so the topmost block ends exactly at the end of
 * usable RAM below 4 GB; on some boards BIOS-reserved memory follows (iGPU
 * stolen memory, SMRAM). A read slightly past a block's end (SSE loads
 * across a line end) must not reach it: such accesses can raise an
 * uncorrectable machine check. With the reserve they hit own memory. */
#define BLOCK_PAD_SMALL 4096u
#define BLOCK_PAD_LARGE 65536u

#ifdef GLIDULATOR_DEBUG
static void log_block(uint32_t addr, uint32_t size) {
    char  line[64], *p = line;
    const char *t = "Memory block ";
    while (*t)
        *p++ = *t++;
    for (int i = 28; i >= 0; i -= 4)
        *p++ = "0123456789ABCDEF"[(addr >> i) & 15];
    *p++ = '-';
    for (int i = 28; i >= 0; i -= 4)
        *p++ = "0123456789ABCDEF"[((addr + size) >> i) & 15];
    *p++ = '\n';
    *p   = 0;
    platform_log(line);
}
#endif

void *platform_alloc(size_t n) {
    if (g_host_alloc)
        return g_host_alloc(n);
    uint32_t handle;
    uint32_t total = (uint32_t) (n + BLOCK_HEADER) + (n >= 65536u ? BLOCK_PAD_LARGE : BLOCK_PAD_SMALL);
    uint8_t *p     = (uint8_t *) dpmi_alloc(total, &handle);
    if (!p)
        return NULL;
#ifdef GLIDULATOR_DEBUG
    if (n >= 65536u)
        log_block((uint32_t) p, total);
#endif
    *(uint32_t *) p = handle;
    return p + BLOCK_HEADER;
}

void platform_free(void *p) {
    if (!p)
        return;
    if (g_host_free) {
        g_host_free(p);
        return;
    }
    dpmi_free(*(uint32_t *) ((uint8_t *) p - BLOCK_HEADER));
}

/* DOS/4GW: flat model without page protection, every DPMI block is
 * executable. */
void *platform_alloc_exec(size_t n) {
    return platform_alloc(n);
}

void platform_free_exec(void *p, size_t n) {
    (void) n;
    platform_free(p);
}

/* platform_time_us(): the CPU's time stamp counter (RDTSC, Pentium and
 * later), calibrated once against the BIOS timer (ticks at 0040:006Ch,
 * 18.2 Hz, linear 46Ch under DOS/4GW). Used by the OSD and the performance
 * log (core/osd.c). Without TSC, or under tools/leload_demo (Linux, no
 * BIOS, recognised by the host memory hook), it stays 0. Calibration takes
 * ~0.2 s at the first call (grSstWinOpen). */
static void rdtsc(uint32_t *lo_hi);
#pragma aux rdtsc = \
    0x0f 0x31       \
    "mov [ecx], eax" \
    "mov [ecx+4], edx" \
    parm [ecx] modify [eax edx];

static uint64_t tsc(void) {
    uint32_t v[2];
    rdtsc(v);
    return ((uint64_t) v[1] << 32) | v[0];
}

static int      g_tsc_state; /* 0 unchecked, 1 calibrated, -1 unavailable */
static uint64_t g_tsc_per_ms;

static void tsc_calibrate(void) {
    volatile const uint32_t *ticks = (volatile const uint32_t *) 0x46c;
    g_tsc_state                    = -1;
    if (g_host_alloc || !(eflags_id_toggles() & 0x200000u) || !(cpuid1_edx() & (1u << 4)))
        return;
    /* Wait for a tick edge, then measure over 3 ticks (~165 ms). If the
     * timer does not run (interrupts off), give up after ~2^31 cycles. */
    uint64_t guard = tsc();
    uint32_t t0    = *ticks;
    while (*ticks == t0)
        if (tsc() - guard > 0x80000000ull)
            return;
    uint64_t c0 = tsc();
    t0          = *ticks;
    while (*ticks - t0 < 3)
        if (tsc() - c0 > 0x400000000ull)
            return;
    uint64_t c1 = tsc();
    /* 3 ticks = 3 * 65536 / 1193182 s = 164.77 ms */
    g_tsc_per_ms = (c1 - c0) * 1000ull / 164775ull;
    if (g_tsc_per_ms)
        g_tsc_state = 1;
}

uint64_t platform_time_us(void) {
    if (!g_tsc_state)
        tsc_calibrate();
    if (g_tsc_state < 0)
        return 0;
    return tsc() * 1000ull / g_tsc_per_ms;
}

/* Vsync through the VGA status register 3DAh (bit 3: vertical retrace),
 * which VBE boards implement too; virtualised in the Windows DOS box. For
 * every retrace the code waits for its start (first the end of a running
 * one, then the start of the next). If none arrives within 100 ms (board
 * without VGA registers), vsync stays off for the session. */
static unsigned inb_3da(void);
#pragma aux inb_3da = "mov dx, 3DAh" "in al, dx" "movzx eax, al" value [eax] modify [edx];

static int g_vretrace_dead;

static int wait_vr_bit(unsigned want) {
    uint64_t t0 = platform_time_us();
    for (unsigned spin = 0;; spin++) {
        if ((inb_3da() & 8u) == want)
            return 1;
        if (!(spin & 1023) && (platform_time_us() - t0 > 100000u || spin > 400000000u))
            return 0;
    }
}

int platform_wait_vretrace(int n) {
    if (g_host_alloc || g_vretrace_dead || !platform_time_us())
        return 0;
    for (int i = 0; i < n; i++)
        if (!wait_vr_bit(0) || !wait_vr_bit(8)) {
            g_vretrace_dead = 1;
            platform_log("Vsync: no retrace at 3DAh, off\n");
            return 0;
        }
    return 1;
}

/* platform_blit() is not here: the VESA output depends on the chosen mode
 * and lives in targets/glide_ovl. */

/* platform_log(): appends to GLIDULAT.LOG in the current directory
 * (trace/debug variants only). Directly via INT 21h from protected mode:
 * DOS/4GW and DOS/32A extend the file functions to flat 32-bit pointers in
 * DS:EDX. Open/write/close per line costs a little but survives a hard
 * crash of the game. */

/* AH=3Dh open (AL=1: write), else AH=3Ch create. Handle or -1. */
static int dos_open_append(const char *name);
#pragma aux dos_open_append = \
    "mov edx, eax"            \
    "mov ax, 3D01h"           \
    "int 21h"                 \
    "jnc short opened"        \
    "mov ah, 3Ch"             \
    "xor ecx, ecx"            \
    "int 21h"                 \
    "jnc short opened"        \
    "mov eax, -1"             \
    "jmp short done"          \
    "opened:"                 \
    "movzx eax, ax"           \
    "push eax"                \
    "mov ebx, eax"            \
    "mov ax, 4202h"           \
    "xor ecx, ecx"            \
    "xor edx, edx"            \
    "int 21h"                 \
    "pop eax"                 \
    "done:"                   \
    parm [eax] value [eax] modify [ebx ecx edx];

/* Close after every line. AH=0Dh (flush all DOS buffers to disk) only for
 * important lines (crash, multi-core, machine check) and otherwise at most
 * once per second: without flushing, directory entry and FAT stay in the
 * buffers, and after a crash the file ends at the last cluster written
 * back (FreeDOS; AH=68h does not help there). Flushing after every line is
 * too much: with many lines in a loading screen, writing to a USB stick
 * through the BIOS, DOS/4GW reported double faults in its mode switch
 * code. */
static void dos_write_close(int handle, const char *buf, unsigned len);
#pragma aux dos_write_close = \
    "mov ah, 40h"             \
    "int 21h"                 \
    "mov ah, 3Eh"             \
    "int 21h"                 \
    parm [ebx] [edx] [ecx] modify [eax];
static void dos_flush(void);
#pragma aux dos_flush = "mov ah, 0Dh" "int 21h" modify [eax];

static int log_important(const char *l) {
    static const char *const keys[] = { "CRASH", "Multi-core", "Machine", "  Bank", "  AP ", "Exception" };
    for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        const char *a = l, *b = keys[k];
        while (*b && *a == *b)
            a++, b++;
        if (!*b)
            return 1;
    }
    return 0;
}

void platform_log(const char *line) {
    static uint64_t last_flush;
    unsigned        len = 0;
    while (line[len])
        len++;
    int h = dos_open_append("GLIDULAT.LOG");
    if (h < 0)
        return;
    dos_write_close(h, line, len);
    uint64_t now = g_tsc_state > 0 ? platform_time_us() : 0;
    if (log_important(line) || !now || now - last_flush > 1000000u) {
        dos_flush();
        last_flush = now;
    }
}

/* platform_write_file(): AH=3Ch create/truncate, AH=40h writes in 32 KB
 * chunks (independent of how much the extender buffers per transfer),
 * AH=3Eh close. */
static int dos_create(const char *name);
#pragma aux dos_create = \
    "mov edx, eax"       \
    "mov ah, 3Ch"        \
    "xor ecx, ecx"       \
    "int 21h"            \
    "jnc short ok"       \
    "mov eax, -1"        \
    "jmp short done"     \
    "ok:"                \
    "movzx eax, ax"      \
    "done:"              \
    parm [eax] value [eax] modify [ecx edx];

/* Returns the number of bytes written, -1 on error. */
static int dos_write(int handle, const void *buf, unsigned len);
#pragma aux dos_write = \
    "mov ah, 40h"       \
    "int 21h"           \
    "jnc short ok"      \
    "mov eax, -1"       \
    "jmp short done"    \
    "ok:"               \
    "movzx eax, ax"     \
    "done:"             \
    parm [ebx] [edx] [ecx] value [eax];

static void dos_close(int handle);
#pragma aux dos_close = \
    "mov ah, 3Eh"       \
    "int 21h"           \
    parm [ebx] modify [eax];

int platform_write_file(const char *name, const void *data, size_t len) {
    int h = dos_create(name);
    if (h < 0)
        return -1;
    const char *p  = (const char *) data;
    int         rc = 0;
    while (len) {
        unsigned chunk = len > 32768u ? 32768u : (unsigned) len;
        if (dos_write(h, p, chunk) != (int) chunk) {
            rc = -1;
            break;
        }
        p += chunk;
        len -= chunk;
    }
    dos_close(h);
    return rc;
}

/* platform_read_file(): AH=3Dh (AL=0) open, AH=42h seek, AH=3Fh reads in
 * 32 KB chunks; as above, DOS/4GW and DOS/32A extend the calls to flat
 * 32-bit pointers. */
static int dos_open_read(const char *name);
#pragma aux dos_open_read = \
    "mov edx, eax"          \
    "mov ax, 3D00h"         \
    "int 21h"               \
    "jnc short ok"          \
    "mov eax, -1"           \
    "jmp short done"        \
    "ok:"                   \
    "movzx eax, ax"         \
    "done:"                 \
    parm [eax] value [eax] modify [edx];

/* Absolute seek (CX:DX = offset). 0 on success, -1 on error. */
static int dos_seek(int handle, unsigned offset);
#pragma aux dos_seek = \
    "mov edx, ecx"     \
    "shr ecx, 16"      \
    "mov ax, 4200h"    \
    "int 21h"          \
    "mov eax, 0"       \
    "jnc short done"   \
    "mov eax, -1"      \
    "done:"            \
    parm [ebx] [ecx] value [eax] modify [ecx edx];

/* Returns the number of bytes read, -1 on error. */
static int dos_read(int handle, void *buf, unsigned len);
#pragma aux dos_read = \
    "mov ah, 3Fh"      \
    "int 21h"          \
    "jnc short ok"     \
    "mov eax, -1"      \
    "jmp short done"   \
    "ok:"              \
    "movzx eax, ax"    \
    "done:"            \
    parm [ebx] [edx] [ecx] value [eax];

long platform_read_file(const char *name, size_t offset, void *buf, size_t len) {
    /* tools/leload_demo (Linux, recognised by the memory hook) has no
     * INT 21h, hence no files. */
    if (g_host_alloc)
        return -1;
    int h = dos_open_read(name);
    if (h < 0)
        return -1;
    long  total = 0;
    char *p     = (char *) buf;
    if (dos_seek(h, (unsigned) offset) == 0) {
        while (len) {
            unsigned chunk = len > 32768u ? 32768u : (unsigned) len;
            int      n     = dos_read(h, p, chunk);
            if (n <= 0)
                break;
            total += n;
            p += n;
            len -= (unsigned) n;
            if ((unsigned) n < chunk)
                break;
        }
    }
    dos_close(h);
    return total;
}

/* platform_get_config(): without a C runtime the OVL cannot easily reach
 * the environment (PSP/environment block as selector or segment depending
 * on the extender), so it reads GLIDULAT.CFG in the current directory
 * (format: glidulator/config.h). */
int platform_get_config(const char *name, char *buf, size_t n) {
    return glide_config_from_file("GLIDULAT.CFG", name, buf, n);
}
