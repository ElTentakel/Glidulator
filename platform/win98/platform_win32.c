/* platform.h implementation for Windows 9x (Win32 subset). Only Win32 API
 * that exists on Windows 98, nothing introduced with NT/2000 or later.
 * Render workers are threads on NT. Windows 9x schedules one core only;
 * there the workers run on the other cores through our own dynamically
 * loaded VxD (GLSMP.VXD next to the DLL, platform/win98/vxd), if present. */

#define _WIN32_WINNT 0x0400
#include <windows.h>

#include "glidulator/config.h"
#include "glidulator/platform.h"
#include "../common/cpu_gcc.h"
#include "../common/output_rect.h"
#include "wc_win9x.h"

#include <string.h>

/* ---- Windows 9x multi-core through GLSMP.VXD ----
 * The VxD starts the other cores (APs) and runs our worker function on them
 * in ring 0, in this process's address space (it copies the process's page
 * directory entries). Everything a worker touches must stay present with
 * unchanged page tables: the VxD locks it (_LinPageLock) -- this DLL's
 * image, msvcrt.dll (memcpy/memset), and every platform_alloc and
 * platform_alloc_exec block (registered from the start, locked once the VxD
 * is up). Worker code must not use FS, SEH, TLS or Win32 calls (it does not;
 * the same code runs without a C runtime in the DOS OVL). Closing the handle,
 * also implicitly when the process ends or is killed, stops the APs and
 * unlocks. No VxD, NT, or GLIDULATOR_SMP=off: one core as before. */
#define VXD_LOCK   8
#define VXD_UNLOCK 9
#define VXD_START  4
#define VXD_RUN    10
#define VXD_JOB    11
#define VXD_JOB_DONE  3
#define VXD_JOB_FAULT 4
#define VXD_MAX_AP    8
#define VXD_MAX_BLOCKS 64

static HANDLE g_vxd = INVALID_HANDLE_VALUE;
static int    g_vxd_state; /* 0 not tried, 1 up, -1 unavailable */
static int    g_vxd_aps;   /* APs running worker slots */
static struct {
    void *p;
    DWORD n;
} g_blocks[VXD_MAX_BLOCKS]; /* every platform_alloc(_exec) block */
static int g_nblocks;
static int g_vxd_jobs; /* workers currently running on APs */

#ifdef GLIDULATOR_TRACE
static void vxd_log(const char *fmt, DWORD a, DWORD b) {
    char line[160];
    wsprintfA(line, fmt, a, b);
    platform_log(line);
}
#else
#    define vxd_log(fmt, a, b) ((void) 0)
#endif

static int vxd_lock(const void *p, DWORD n, int code) {
    DWORD in[2] = {(DWORD) p, n}, st = 99, r = 0;
    return DeviceIoControl(g_vxd, (DWORD) code, in, sizeof in, &st, sizeof st, &r, NULL) && st == 0;
}

static int vxd_lock_image(const void *base) {
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *) base;
    const IMAGE_NT_HEADERS *nt  = (const IMAGE_NT_HEADERS *) ((const BYTE *) base + dos->e_lfanew);
    return vxd_lock(base, nt->OptionalHeader.SizeOfImage, VXD_LOCK);
}

static void block_add(void *p, size_t n) {
    if (!p)
        return;
    /* The APs cache translations Windows never flushes on them (they may
     * even load them speculatively): memory mapped while workers run can be
     * seen at its old physical address there. core/glide_abi.c allocates
     * before it starts the workers; this catches violations. */
    if (g_vxd_jobs)
        vxd_log("Multi-core: allocation at %08lX (%lu bytes) while VxD workers run\n", (DWORD) p, (DWORD) n);
    if (g_nblocks < VXD_MAX_BLOCKS) {
        g_blocks[g_nblocks].p   = p;
        g_blocks[g_nblocks++].n = (DWORD) n;
    } else if (!g_vxd_state)
        g_vxd_state = -1; /* could not lock it later: stay on one core */
    if (g_vxd_state > 0 && !vxd_lock(p, (DWORD) n, VXD_LOCK))
        vxd_log("Multi-core: VxD lock of %08lX failed\n", (DWORD) p, 0);
}

static void block_remove(void *p) {
    for (int i = 0; i < g_nblocks; i++)
        if (g_blocks[i].p == p) {
            if (g_vxd_state > 0)
                vxd_lock(p, g_blocks[i].n, VXD_UNLOCK);
            g_blocks[i] = g_blocks[--g_nblocks];
            return;
        }
}

/* Load the VxD, lock what workers touch, start the APs. Once. */
static int vxd_up(void) {
    char  dir[MAX_PATH], path[MAX_PATH + 8], cfg[8];
    DWORD in[2] = {0, 0}, st[10] = {0}, r = 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (g_vxd_state)
        return g_vxd_state > 0;
    g_vxd_state = -1;
    if (!(GetVersion() & 0x80000000u)) /* NT: threads */
        return 0;
    if (platform_get_config("GLIDULATOR_SMP", cfg, sizeof cfg) && !strcmp(cfg, "off"))
        return 0;
    if (platform_get_config("GLIDULATOR_THREADS", cfg, sizeof cfg) && cfg[0] == '1' && !cfg[1])
        return 0; /* one core wanted: do not start any */
    if (!VirtualQuery(&g_vxd, &mbi, sizeof mbi) || /* an address in this DLL */
        !GetModuleFileNameA((HMODULE) mbi.AllocationBase, dir, sizeof dir))
        return 0;
    char *slash = strrchr(dir, '\\');
    if (!slash)
        return 0;
    strcpy(slash + 1, "GLSMP.VXD");
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES ||
        !GetShortPathNameA(dir, path + 4, MAX_PATH))
        return 0; /* no VxD next to the DLL: one core */
    memcpy(path, "\\\\.\\", 4);
    g_vxd = CreateFileA(path, 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (g_vxd == INVALID_HANDLE_VALUE) {
        vxd_log("Multi-core: VxD did not load (error %lu)\n", GetLastError(), 0);
        return 0;
    }
    HMODULE crt = GetModuleHandleA("msvcrt.dll");
    int     ok  = vxd_lock_image(mbi.AllocationBase) && (!crt || vxd_lock_image(crt));
    for (int i = 0; ok && i < g_nblocks; i++)
        ok = vxd_lock(g_blocks[i].p, g_blocks[i].n, VXD_LOCK);
    if (ok)
        ok = DeviceIoControl(g_vxd, VXD_START, in, sizeof in, st, sizeof st, &r, NULL) && r >= 40 && st[5] == 0 &&
             st[6] > 0;
    if (!ok) {
        vxd_log("Multi-core: VxD start failed (status %lu, %lu usable APs)\n", st[5], st[6]);
        CloseHandle(g_vxd); /* the VxD unlocks and stops what it did */
        g_vxd = INVALID_HANDLE_VALUE;
        return 0;
    }
    /* st[6]: APs with a worker slot, one per physical core except the
     * main core's; Hyper-Threading siblings (st[7]) stay parked */
    g_vxd_aps   = st[6] > VXD_MAX_AP ? VXD_MAX_AP : (int) st[6];
    g_vxd_state = 1;
    vxd_log("Multi-core: Windows 9x VxD up, %lu additional cores (%lu Hyper-Threading siblings unused)\n",
            (DWORD) g_vxd_aps, st[7]);
    return 1;
}

/* NT/2000/XP report all cores. Windows 9x always reports 1 (no SMP
 * kernel); there the VxD's APs count. */
int platform_core_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (si.dwNumberOfProcessors > 1)
        return (int) si.dwNumberOfProcessors;
    return vxd_up() ? 1 + g_vxd_aps : 1;
}

const char *platform_smp_mode(void) {
    return g_vxd_state > 0 ? "GLSMP" : !(GetVersion() & 0x80000000u) ? "THREADS" : 0;
}

/* SSE needs the OS to save the XMM registers (FXSAVE, CR4.OSFXSR).
 * Windows 98SE and ME do; Windows 95 and the first Windows 98 do not, and
 * there every SSE instruction raises an invalid-opcode exception even if
 * CPUID reports SSE. So one SSE instruction runs behind an own SEH frame
 * (the innermost one, ahead of any handler of the game) that skips it on
 * a fault. Without SSE the JIT is off and the C path renders. Only the
 * JIT emits SSE; the DLL itself is built without -msse. */
static volatile LONG g_probe_fault;
static volatile LONG g_probe_skip; /* length of the probed instruction */

static EXCEPTION_DISPOSITION __cdecl probe_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *disp) {
    (void) frame;
    (void) disp;
    if (!g_probe_skip || (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) ||
        (rec->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION && rec->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION))
        return ExceptionContinueSearch;
    g_probe_fault = 1;
    ctx->Eip += (DWORD) g_probe_skip;
    return ExceptionContinueExecution;
}

static int sse_usable(void) {
    struct {
        void *prev;
        EXCEPTION_DISPOSITION(__cdecl *handler)(EXCEPTION_RECORD *, void *, CONTEXT *, void *);
    } reg; /* SEH registration: must lie on the thread's stack */
    int ud = 0;
#ifdef GLIDULATOR_DEBUG
    {   /* test of the fallback: GLIDULATOR_SSE_TEST=ud probes ud2 instead */
        char cfg[8];
        ud = platform_get_config("GLIDULATOR_SSE_TEST", cfg, sizeof cfg) && cfg[0] == 'u';
    }
#endif
    g_probe_fault = 0;
    g_probe_skip  = ud ? 2 : 3;
    reg.handler   = probe_handler;
    if (ud)
        __asm__ volatile("movl %%fs:0, %%eax\n\t"
                         "movl %%eax, (%0)\n\t"
                         "movl %0, %%fs:0\n\t"
                         ".byte 0x0f, 0x0b\n\t" /* ud2 */
                         "movl (%0), %%eax\n\t"
                         "movl %%eax, %%fs:0"
                         :
                         : "r"(&reg)
                         : "eax", "memory");
    else
        __asm__ volatile("movl %%fs:0, %%eax\n\t"
                         "movl %%eax, (%0)\n\t"
                         "movl %0, %%fs:0\n\t"
                         ".byte 0x0f, 0x57, 0xc0\n\t" /* xorps xmm0, xmm0 */
                         "movl (%0), %%eax\n\t"
                         "movl %%eax, %%fs:0"
                         :
                         : "r"(&reg)
                         : "eax", "memory");
    g_probe_skip = 0;
    return !g_probe_fault;
}

unsigned platform_cpu_features(void) {
    static int os_sse = -1; /* checked once */
    if (os_sse < 0)
        os_sse = sse_usable();
    return cpu_features_gcc(os_sse);
}

/* Own heap instead of the process heap: an overrun in either the DLL or the
 * game then cannot corrupt the other's heap blocks, and a heap crash can be
 * attributed by its heap handle. Created on first use (also from DllMain,
 * where HeapCreate is allowed); released with the process. */
static HANDLE g_heap;

static HANDLE heap(void) {
    if (!g_heap) {
        g_heap = HeapCreate(0, 0, 0);
        if (!g_heap) /* decided once: blocks must be freed on the heap they came from */
            g_heap = GetProcessHeap();
    }
    return g_heap;
}

void *platform_alloc(size_t n) {
    void *p = HeapAlloc(heap(), 0, n);
    block_add(p, n);
    return p;
}

void platform_free(void *p) {
    if (p) {
        block_remove(p);
        HeapFree(heap(), 0, p);
    }
}

void *platform_alloc_exec(size_t n) {
    void *p = VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    block_add(p, n);
    return p;
}

void platform_free_exec(void *p, size_t n) {
    (void) n;
    if (p) {
        block_remove(p);
        VirtualFree(p, 0, MEM_RELEASE);
    }
}

uint64_t platform_time_us(void) {
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart * 1000000ull / freq.QuadPart);
}

struct platform_worker {
    HANDLE             thread; /* NULL: runs on VxD AP ap */
    int                ap;
    int                killed;
    platform_worker_fn fn;
    void              *arg;
};

static DWORD vxd_job_state(int ap) {
    DWORD in = (DWORD) ap, out[5] = {0, 0, 0, 0, 0}, r = 0;
    DeviceIoControl(g_vxd, VXD_JOB, &in, sizeof in, out, sizeof out, &r, NULL);
    return out[0];
}

/* fn(arg) on VxD AP core_id - 1, with this thread's x87 control word and
 * MXCSR (results bit-identical to rendering on this core). */
static platform_worker *vxd_spawn(platform_worker *w, int core_id) {
    WORD  cw;
    DWORD mxcsr = 0x1f80, st = 99, r = 0;
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    if (platform_cpu_features() & PLATFORM_CPU_SSE)
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    DWORD in[5] = {(DWORD) (core_id - 1), (DWORD) w->fn, (DWORD) w->arg, cw, mxcsr};
    w->ap       = core_id - 1;
    if (!DeviceIoControl(g_vxd, VXD_RUN, in, sizeof in, &st, sizeof st, &r, NULL) || st) {
        vxd_log("Multi-core: VxD refused worker %lu (status %lu)\n", (DWORD) core_id, st);
        HeapFree(GetProcessHeap(), 0, w);
        return NULL;
    }
    g_vxd_jobs++;
    return w;
}

static DWORD WINAPI worker_main(LPVOID p) {
    platform_worker *w = (platform_worker *) p;
    w->fn(w->arg);
    return 0;
}

platform_worker *platform_spawn_worker(int core_id, platform_worker_fn fn, void *arg) {
    platform_worker *w = (platform_worker *) HeapAlloc(GetProcessHeap(), 0, sizeof *w);
    DWORD            tid;
    (void) core_id;
    if (!w)
        return NULL;
    w->fn     = fn;
    w->arg    = arg;
    w->killed = 0;
    w->thread = NULL;
    if (g_vxd_state > 0)
        return core_id >= 1 && core_id <= g_vxd_aps ? vxd_spawn(w, core_id) : (HeapFree(GetProcessHeap(), 0, w), NULL);
    w->thread = CreateThread(NULL, 0, worker_main, w, 0, &tid);
    if (!w->thread) {
        HeapFree(GetProcessHeap(), 0, w);
        return NULL;
    }
    return w;
}

void platform_join_worker(platform_worker *w) {
    if (!w)
        return;
    if (w->thread) {
        WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
    } else {
        if (!w->killed)
            for (int spin = 0; vxd_job_state(w->ap) < VXD_JOB_DONE; spin++)
                Sleep(spin < 16 ? 0 : 1);
        g_vxd_jobs--;
    }
    HeapFree(GetProcessHeap(), 0, w);
}

/* Threads do not die individually, and none can be aborted cleanly. A VxD
 * worker dies on a CPU exception (the VxD parks its core). */
int platform_worker_failed(platform_worker *w) {
    return w && !w->thread && vxd_job_state(w->ap) == VXD_JOB_FAULT;
}

/* A hung VxD worker is abandoned: its core keeps spinning until the handle
 * is closed (process end), which resets it by INIT; its slot stays busy, so
 * that core takes no new worker. */
int platform_kill_worker(platform_worker *w) {
    if (!w || w->thread)
        return 0;
    w->killed = 1;
    return 1;
}

/* Vsync: not implemented (would need DirectDraw instead of GDI) */
int platform_wait_vretrace(int n) {
    (void) n;
    return 0;
}

/* --- Frame output (GDI) ---------------------------------------------
 *
 * A Voodoo Graphics has its own video output: once grSstWinOpen runs, the
 * monitor shows its picture full-screen at the Glide resolution, without
 * mouse cursor, regardless of the size of the program's window (which only
 * serves keyboard and mouse; some programs use a small one). Reproduced by:
 *
 * - Screen mode per GLIDULATOR_DISPLAY: "glide" (default: the Glide
 *   resolution, 16 bit preferred, else 32 bit), "desktop" (do not switch)
 *   or WxH / WxHxBPP, e.g. 1280x720
 *   for 16:9 monitors that distort 4:3 modes. If switching fails, the
 *   desktop mode stays. Switching to the game resolution avoids scaling to
 *   the full desktop in 32 bit, which is slow with simple VESA drivers.
 * - Picture inside it per GLIDULATOR_SCALE (platform/common/output_rect.h):
 *   "aspect" (default, 4:3 with black borders), "stretch", "integer",
 *   "none" (1:1). Borders are painted only after opening and on WM_PAINT,
 *   not per frame.
 * - An own full-screen window owned by the program's window: always on
 *   top, never activated, transparent to mouse clicks.
 * - The mouse cursor is hidden while Glide is open (otherwise a software
 *   cursor flickers, overwritten by every frame).
 *
 * GDI exists on every Windows 9x (and under Wine) and does not interfere
 * with the program, unlike an exclusive DirectDraw mode.
 *
 * The framebuffer is RGB565; output converts it through 65536-entry tables
 * (gamma-corrected like the Voodoo RAMDAC) to RGB565 (16-bit screen) or
 * XRGB8888 (otherwise). */

static HWND      g_hwnd;
static int       g_w, g_h;
static void     *g_outbuf;      /* output buffer w x h, 2 or 4 bytes per pixel */
static uint32_t *g_conv32;      /* RGB565 -> XRGB8888 incl. gamma */
static uint16_t *g_conv16;      /* RGB565 -> RGB565 incl. gamma */
static uint8_t   g_gamma[256];
static int       g_gamma_set;
static int       g_mode_changed; /* screen mode switched */
static int       g_cursor_hidden; /* number of ShowCursor(FALSE) calls */
static int       g_scale;         /* OUT_SCALE_* */
static int       g_need_border;   /* repaint the black borders */
static int       g_prefer_deep;   /* video filter: 32-bit mode first */

static uint32_t gamma8(uint32_t v) {
    return g_gamma_set ? g_gamma[v] : v;
}

static void build_conv_tables(void) {
    if (!g_conv32)
        g_conv32 = platform_alloc(65536 * sizeof *g_conv32);
    if (!g_conv16)
        g_conv16 = platform_alloc(65536 * sizeof *g_conv16);
    if (!g_conv32 || !g_conv16)
        return;
    for (unsigned i = 0; i < 65536; i++) {
        unsigned r5 = i >> 11, g6 = (i >> 5) & 63, b5 = i & 31;
        unsigned r = r5 << 3 | r5 >> 2, g = g6 << 2 | g6 >> 4, b = b5 << 3 | b5 >> 2;
        if (g_gamma_set) {
            r = g_gamma[r];
            g = g_gamma[g];
            b = g_gamma[b];
        }
        g_conv32[i] = (uint32_t) (r << 16 | g << 8 | b);
        g_conv16[i] = (uint16_t) ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
    }
}

#ifdef GLIDULATOR_TRACE
static void log_window(const char *what, HWND w);
#endif

/* GLQuake 0.97 minimizes its full-screen window whenever it loses the
 * focus, and on every start it shows a progress dialog for a moment
 * ("Creating 15 bit inverse palette") once the GL context exists. When
 * the dialog closes, GLQuake is activated again but still minimized,
 * takes that as "minimized" and stops drawing (no more grBufferSwap)
 * until a key opens its menu. A timer on our window (the game's thread
 * dispatches its messages) restores the program window when it is
 * minimized although it is still in front; a program the user switched
 * away from is not in front and stays minimized. */
#define RESTORE_TIMER 1

static void restore_program_window(void) {
    HWND w = GetActiveWindow();
    if (!w || !IsIconic(w) || w != GetForegroundWindow())
        return;
#ifdef GLIDULATOR_TRACE
    log_window("Program window minimized in front, restoring:", w);
#endif
    ShowWindow(w, SW_RESTORE);
}

static LRESULT CALLBACK own_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SETCURSOR) {
        SetCursor(NULL);
        return TRUE;
    }
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;
    if (msg == WM_PAINT) {
        ValidateRect(hwnd, NULL);
        g_need_border = 1;
        return 0;
    }
    if (msg == WM_TIMER && wp == RESTORE_TIMER) {
        restore_program_window();
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Owner = the program's active window (thread of grSstWinOpen): an owned
 * window always stays above its owner, even under a window manager that
 * would otherwise raise the program's active window above a "topmost"
 * window (e.g. a DirectDraw full-screen window). */
static HWND create_own_window(void) {
    WNDCLASSA wc;
    HINSTANCE inst = GetModuleHandleA(NULL);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = own_wndproc;
    wc.hInstance     = inst;
    wc.hbrBackground = (HBRUSH) GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "GlidulatorOutput";
    RegisterClassA(&wc); /* fails harmlessly if already registered */
    HWND hwnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, wc.lpszClassName,
                                "Glidulator", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                                GetSystemMetrics(SM_CYSCREEN), NULL, NULL, inst, NULL);
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetTimer(hwnd, RESTORE_TIMER, 200, NULL);
    }
    return hwnd;
}

#ifdef GLIDULATOR_TRACE
/* State of the program window at grSstWinOpen. Wrappers such as the 3dfx
 * MiniGL size the Glide screen and the clip window from its client
 * rectangle; a minimized window (0 x 0) gives a permanently black
 * picture. */
/* what: at most 128 characters are logged (keep_on_top passes up to 126).
 * wsprintfA has no size argument: line must hold the longest result. */
static void log_window(const char *what, HWND w) {
    char line[256];
    RECT r = { 0, 0, 0, 0 };
    if (w)
        GetClientRect(w, &r);
    wsprintfA(line, "%.128s %08lx: minimized %d, visible %d, foreground %d, client %ld x %ld\n", what,
              (unsigned long) (UINT_PTR) w, w ? (int) IsIconic(w) : -1, w ? (int) IsWindowVisible(w) : -1,
              w && w == GetForegroundWindow(), r.right - r.left, r.bottom - r.top);
    platform_log(line);
}

/* Focus, activation and size messages of every window of the game's
 * thread (the thread of grSstWinOpen), to find out who takes the focus.
 * WM_KILLFOCUS: wParam is the window that gets it. */
static HHOOK g_trace_hook;

static LRESULT CALLBACK trace_hook_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const CWPSTRUCT *m = (const CWPSTRUCT *) lp;
        if (m->message == WM_ACTIVATE || m->message == WM_ACTIVATEAPP || m->message == WM_SETFOCUS ||
            m->message == WM_KILLFOCUS || m->message == WM_SIZE || m->message == WM_SHOWWINDOW ||
            m->message == WM_SYSCOMMAND || m->message == WM_CANCELMODE || m->message == WM_CAPTURECHANGED) {
            char line[128], cls[32] = "";
            GetClassNameA(m->hwnd, cls, sizeof cls);
            wsprintfA(line, "%lu ms: message %04x to %08lx (%s), wParam %08lx, lParam %08lx\n",
                      (unsigned long) (platform_time_us() / 1000), m->message, (unsigned long) (UINT_PTR) m->hwnd,
                      cls, (unsigned long) m->wParam, (unsigned long) m->lParam);
            platform_log(line);
        }
    }
    return CallNextHookEx(g_trace_hook, code, wp, lp);
}
#endif

/* Our window has no owner on purpose: Windows hides owned windows while
 * their owner is minimized, and GLQuake (3dfx MiniGL) minimizes its
 * full-screen window whenever it loses the focus -- the picture vanished
 * and the desktop showed. Like a real Voodoo, the output must not depend
 * on the program window. Topmost keeps it above the program; it is
 * re-asserted every 30 frames because some programs raise their own
 * window (GLQuake in windowed mode, and Wine's X stacking), and shown
 * again should anything hide it. */
static void keep_on_top(void) {
    static unsigned frames;
#ifdef GLIDULATOR_TRACE
    {
        /* Log every change of the foreground window and its state. */
        static HWND last_w;
        static int  last_state = -1;
        static HWND last_focus, last_active;
        HWND        w     = GetForegroundWindow();
        HWND        focus = GetFocus(), active = GetActiveWindow();
        int         state = w ? (IsIconic(w) ? 1 : 0) | (IsWindowVisible(w) ? 2 : 0) : -1;
        if (w != last_w || state != last_state || focus != last_focus || active != last_active) {
            char line[128], cls[32] = "";
            if (w)
                GetClassNameA(w, cls, sizeof cls);
            wsprintfA(line, "%lu ms, frame %u: focus %08lx, active %08lx, ours %08lx, foreground (%s)",
                      (unsigned long) (platform_time_us() / 1000), frames,
                      (unsigned long) (UINT_PTR) focus, (unsigned long) (UINT_PTR) active,
                      (unsigned long) (UINT_PTR) g_hwnd, cls);
            log_window(line, w);
            last_w      = w;
            last_state  = state;
            last_focus  = focus;
            last_active = active;
        }
    }
#endif
    if (frames++ % 30)
        return;
    if (!IsWindowVisible(g_hwnd)) {
#ifdef GLIDULATOR_TRACE
        log_window("Output window was hidden, program window", GetForegroundWindow());
#endif
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    }
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static int set_mode(int w, int h, int bpp) {
    DEVMODEA dm;
    memset(&dm, 0, sizeof dm);
    dm.dmSize       = sizeof dm;
    dm.dmPelsWidth  = (DWORD) w;
    dm.dmPelsHeight = (DWORD) h;
    dm.dmBitsPerPel = (DWORD) bpp;
    dm.dmFields     = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    return ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN) == DISP_CHANGE_SUCCESSFUL;
}

#ifdef GLIDULATOR_TRACE
static void log_mode(const char *what) {
    char line[96];
    HDC  dc  = GetDC(NULL);
    int  bpp = GetDeviceCaps(dc, BITSPIXEL);
    ReleaseDC(NULL, dc);
    wsprintfA(line, "Output: %s %d x %d, %d bit\n", what, GetSystemMetrics(SM_CXSCREEN),
              GetSystemMetrics(SM_CYSCREEN), bpp);
    platform_log(line);
}

#endif

/* 16 bit first (565 without conversion), 32 bit first with the video filter */
static int try_modes(int w, int h) {
    if (g_prefer_deep)
        return set_mode(w, h, 32) || set_mode(w, h, 16);
    return set_mode(w, h, 16) || set_mode(w, h, 32);
}

int platform_display_size(int *w, int *h) {
    if (!g_hwnd)
        return 0;
    *w = GetSystemMetrics(SM_CXSCREEN);
    *h = GetSystemMetrics(SM_CYSCREEN);
    return 1;
}

void platform_display_open(int w, int h) {
    char cfg[24];
    platform_display_close();
    g_w      = w;
    g_h      = h;
    g_outbuf = platform_alloc((size_t) w * (size_t) h * 4);
    build_conv_tables();
    g_mode_changed = 0;
    if (platform_get_config("GLIDULATOR_DISPLAY", cfg, sizeof cfg)) {
        int dw = 0, dh = 0, db = 0;
        output_display_parse(cfg, &dw, &dh, &db);
        if (dw > 0 && dh > 0)
            g_mode_changed = db ? set_mode(dw, dh, db) : try_modes(dw, dh);
        else if (cfg[0] != 'd' && cfg[0] != 'D') /* "glide" */
            g_mode_changed = try_modes(w, h);
    } else {
        g_mode_changed = try_modes(w, h);
    }
    g_scale = platform_get_config("GLIDULATOR_SCALE", cfg, sizeof cfg) ? output_scale_parse(cfg) : OUT_SCALE_ASPECT;
    if (g_scale < 0)
        g_scale = OUT_SCALE_ASPECT;
    g_need_border = 1;
#ifdef GLIDULATOR_TRACE
    log_mode(g_mode_changed ? "Mode switched," : "Mode unchanged, screen");
    log_window("Program window (active)", GetActiveWindow());
    if (!g_trace_hook)
        g_trace_hook = SetWindowsHookExA(WH_CALLWNDPROC, trace_hook_proc, NULL, GetCurrentThreadId());
#endif
    g_hwnd = create_own_window();
    while (ShowCursor(FALSE) >= 0 && g_cursor_hidden < 100)
        g_cursor_hidden++;
    g_cursor_hidden++; /* the last call counted as well */

    /* Write-combining for the display adapter's framebuffer (ring 0, only
     * generic drivers under Windows 9x; GLIDULATOR_WC=0 to skip). Size it
     * from the current screen mode -- that is what the driver copies into. */
    {
        HDC      dc  = GetDC(NULL);
        unsigned bpp = (unsigned) GetDeviceCaps(dc, BITSPIXEL);
        ReleaseDC(NULL, dc);
        uint32_t need = (uint32_t) GetSystemMetrics(SM_CXSCREEN) * (uint32_t) GetSystemMetrics(SM_CYSCREEN) *
                        ((bpp + 7) / 8);
        wc_win9x_on(need);
    }
}

void platform_display_close(void) {
    wc_win9x_off();
#ifdef GLIDULATOR_TRACE
    if (g_trace_hook)
        UnhookWindowsHookEx(g_trace_hook);
    g_trace_hook = NULL;
#endif
    if (g_hwnd)
        DestroyWindow(g_hwnd);
    g_hwnd = NULL;
    for (; g_cursor_hidden > 0; g_cursor_hidden--)
        ShowCursor(TRUE);
    if (g_mode_changed)
        ChangeDisplaySettingsA(NULL, 0);
    g_mode_changed = 0;
    platform_free(g_outbuf);
    g_outbuf = NULL;
}

int platform_display_lost(void) {
    return 0;
}

/* Windows has its own Print Screen (desktop to the clipboard). */
int platform_screenshot_key(void) {
    return 0;
}

#ifdef GLIDULATOR_TRACE
/* Timing on real hardware: every 100 frames the frame rate and the output
 * cost per frame, split into conversion (table, in system RAM) and GDI
 * (copy/scale into video memory). */
static void trace_timing(uint64_t t0, uint64_t t1, uint64_t t2, int bpp, int x, int y, int rw, int rh) {
    static uint64_t start, conv_sum, gdi_sum;
    static unsigned frames;
    char            line[160];
    if (!frames)
        start = t0;
    conv_sum += t1 - t0;
    gdi_sum += t2 - t1;
    if (++frames == 100) {
        uint64_t dt = t2 - start;
        wsprintfA(line,
                  "Output: %u.%u fps, output %u us/frame (convert %u, GDI %u), screen %d x %d, %d bit, "
                  "picture %d x %d at %d,%d\n",
                  (unsigned) (100000000ull * 10 / (dt ? dt : 1) / 10),
                  (unsigned) (100000000ull * 10 / (dt ? dt : 1) % 10), (unsigned) ((conv_sum + gdi_sum) / 100),
                  (unsigned) (conv_sum / 100), (unsigned) (gdi_sum / 100), GetSystemMetrics(SM_CXSCREEN),
                  GetSystemMetrics(SM_CYSCREEN), bpp, rw, rh, x, y);
        platform_log(line);
        frames   = 0;
        conv_sum = gdi_sum = 0;
    }
}
#endif

/* Black borders around the target rectangle (only if g_need_border). */
static void fill_border(HDC dc, const RECT *rc, int x, int y, int rw, int rh) {
    HBRUSH black = (HBRUSH) GetStockObject(BLACK_BRUSH);
    RECT   r;
    SetRect(&r, 0, 0, rc->right, y);
    FillRect(dc, &r, black);
    SetRect(&r, 0, y + rh, rc->right, rc->bottom);
    FillRect(dc, &r, black);
    SetRect(&r, 0, y, x, y + rh);
    FillRect(dc, &r, black);
    SetRect(&r, x + rw, y, rc->right, y + rh);
    FillRect(dc, &r, black);
}

/* src32: source XRGB8888 (video filter), otherwise RGB565 */
static void blit_any(const void *pixels, int w, int h, int stride_bytes, int src32) {
    struct {
        BITMAPINFOHEADER h;
        DWORD            masks[3];
    } bmi;
    RECT rc;
    HDC  dc;
    int  bpp, x, y, rw, rh;
#ifdef GLIDULATOR_TRACE
    uint64_t t0 = platform_time_us(), t1;
#endif
    if (!g_hwnd || !g_outbuf || !g_conv32 || !g_conv16 || w != g_w || h != g_h)
        return;
    /* Only our own window's messages: some programs do not pump messages
     * while rendering, and the window would never be shown. The
     * application's messages stay untouched. */
    {
        MSG msg;
        while (PeekMessageA(&msg, g_hwnd, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
    }
    keep_on_top();
    if (!GetClientRect(g_hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0)
        return;
    dc = GetDC(g_hwnd);
    if (!dc)
        return;
    bpp = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);
    memset(&bmi, 0, sizeof bmi);
    bmi.h.biSize   = sizeof bmi.h;
    bmi.h.biWidth  = w;
    bmi.h.biHeight = -h; /* top-down */
    bmi.h.biPlanes = 1;
    if (bpp == 15 || bpp == 16) {
        /* 16-bit screen: 565 stays 565, GDI converts nothing */
        for (int j = 0; j < h; j++) {
            const uint8_t *row = (const uint8_t *) pixels + (size_t) j * (size_t) stride_bytes;
            uint16_t      *dst = (uint16_t *) g_outbuf + (size_t) j * (size_t) w;
            if (src32)
                for (int i = 0; i < w; i++) {
                    uint32_t q = ((const uint32_t *) row)[i];
                    uint32_t r = gamma8((q >> 16) & 0xff), g = gamma8((q >> 8) & 0xff), b = gamma8(q & 0xff);
                    dst[i]     = (uint16_t) ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
                }
            else
                for (int i = 0; i < w; i++)
                    dst[i] = g_conv16[((const uint16_t *) row)[i]];
        }
        bmi.h.biBitCount    = 16;
        bmi.h.biCompression = BI_BITFIELDS;
        bmi.masks[0]        = 0xf800;
        bmi.masks[1]        = 0x07e0;
        bmi.masks[2]        = 0x001f;
    } else {
        for (int j = 0; j < h; j++) {
            const uint8_t *row = (const uint8_t *) pixels + (size_t) j * (size_t) stride_bytes;
            uint32_t      *dst = (uint32_t *) g_outbuf + (size_t) j * (size_t) w;
            if (src32)
                for (int i = 0; i < w; i++) {
                    uint32_t q = ((const uint32_t *) row)[i];
                    dst[i]     = gamma8((q >> 16) & 0xff) << 16 | gamma8((q >> 8) & 0xff) << 8 | gamma8(q & 0xff);
                }
            else
                for (int i = 0; i < w; i++)
                    dst[i] = g_conv32[((const uint16_t *) row)[i]];
        }
        bmi.h.biBitCount    = 32;
        bmi.h.biCompression = BI_RGB;
    }
#ifdef GLIDULATOR_TRACE
    t1 = platform_time_us();
#endif
    output_rect(g_scale, w, h, rc.right, rc.bottom, &x, &y, &rw, &rh);
    if (g_need_border) {
        fill_border(dc, &rc, x, y, rw, rh);
        g_need_border = 0;
    }
    if (rw != w || rh != h) {
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, x, y, rw, rh, 0, 0, w, h, g_outbuf, (BITMAPINFO *) &bmi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        SetDIBitsToDevice(dc, x, y, (DWORD) w, (DWORD) h, 0, 0, 0, (UINT) h, g_outbuf, (BITMAPINFO *) &bmi,
                          DIB_RGB_COLORS);
    }
    ReleaseDC(g_hwnd, dc);
#ifdef GLIDULATOR_TRACE
    trace_timing(t0, t1, platform_time_us(), bpp, x, y, rw, rh);
#endif
}

void platform_blit(const void *pixels, int w, int h, int stride_bytes) {
    blit_any(pixels, w, h, stride_bytes, 0);
}

void platform_blit32(const uint32_t *pixels, int w, int h, int stride_bytes) {
    blit_any(pixels, w, h, stride_bytes, 1);
}

/* GLQuake in full screen minimizes itself on WM_KILLFOCUS, and its own
 * start-up dialog takes the focus right before the 3dfx MiniGL creates the
 * GL context. The MiniGL then reads a 0 x 0 client area, picks 512 x 384
 * and clips everything away (grClipWindow(0,0,0,0)) for the rest of the
 * session. The MiniGL calls grSstSelect() immediately before reading the
 * client area, so restoring the window here gives it the real size. A
 * program that is not minimized is not affected. */
void platform_prepare_window(void) {
    HWND w = GetActiveWindow();
    if (!w || !IsIconic(w))
        return;
#ifdef GLIDULATOR_TRACE
    log_window("Program window minimized, restoring:", w);
#endif
    ShowWindow(w, SW_RESTORE);
}

void platform_display_prefer_deep(int on) {
    g_prefer_deep = on;
}

void platform_set_gamma(const uint8_t lut[256]) {
    memcpy(g_gamma, lut, sizeof g_gamma);
    g_gamma_set = 1;
    if (g_conv32)
        build_conv_tables();
}

/* Environment variable first (like 3dfx's driver), then GLIDULAT.CFG in
 * the current directory as for the OVL; on Windows 98 more convenient than
 * SET lines in AUTOEXEC.BAT. */
int platform_get_config(const char *name, char *buf, size_t n) {
    DWORD len = GetEnvironmentVariableA(name, buf, (DWORD) n);
    if (len > 0 && len < n)
        return 1;
    return glide_config_from_file("GLIDULAT.CFG", name, buf, n);
}

/* Only the trace/debug DLL logs: like the OVL into GLIDULAT.LOG in the
 * current directory, created anew at the first call of the process,
 * appended afterwards (survives a crash). */
void platform_log(const char *line) {
    static int started;
    DWORD      done;
    HANDLE     h = CreateFileA("GLIDULAT.LOG", GENERIC_WRITE, FILE_SHARE_READ, NULL,
                               started ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    OutputDebugStringA(line);
    if (h == INVALID_HANDLE_VALUE)
        return;
    started = 1;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, line, (DWORD) strlen(line), &done, NULL);
    CloseHandle(h);
}

int platform_write_file(const char *name, const void *data, size_t len) {
    HANDLE h = CreateFileA(name, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD  done = 0;
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    BOOL ok = WriteFile(h, data, (DWORD)len, &done, NULL);
    CloseHandle(h);
    return (ok && done == len) ? 0 : -1;
}

long platform_read_file(const char *name, size_t offset, void *buf, size_t len) {
    HANDLE h = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD  done = 0;
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    if (SetFilePointer(h, (LONG) offset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER ||
        !ReadFile(h, buf, (DWORD) len, &done, NULL))
        done = 0;
    CloseHandle(h);
    return (long) done;
}
