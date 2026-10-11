/* Multi-core rendering: triangles go into a queue; n workers on other
 * cores draw them in the same order, worker i the scanlines with
 * y % n == i (vid_voodoo_render.c, like 86Box's render threads). The game
 * continues on its own core; it waits only at synchronisation points
 * (glide_mt_idle: swap, clear, LFB, full texture cache, close).
 *
 * One producer (the Glide calls), n consumers that each read ALL entries:
 * g_q_write counts queued triangles, g_q_done[i] those worker i has drawn.
 * On x86 (TSO) store order is sufficient: write the entry, then increment
 * g_q_write; a worker reads g_q_write, then the entry. No C runtime (also
 * runs in the OVL).
 *
 * Safety net: if a worker dies (platform_worker_failed: CPU exception on
 * its core under DOS) or makes no progress for 2 s while the main core
 * waits for it (platform_kill_worker), mt_recover() saves the frame: the
 * other workers finish the queue, the main core redraws the lost worker's
 * scanlines (without the triangle it stopped on), the texture-cache
 * counters are reconciled, and rendering continues on one core until the
 * DLL/OVL is reloaded. Without it the game would hang in glide_mt_idle()
 * forever. */

#include "glidulator/platform.h"
#include "glidulator/renderer.h"

#include "glide_internal.h"
#include "vid_voodoo_render.h"

#define MT_QUEUE 256 /* power of two */

typedef struct {
    voodoo_params_t params;
} mt_entry;

static mt_entry          *g_q;
static volatile uint32_t  g_q_write;
static volatile uint32_t  g_q_done[GLIDE_MT_MAX];
static volatile int       g_quit;
static int                g_workers;
static int                g_ids[GLIDE_MT_MAX];
static platform_worker   *g_w[GLIDE_MT_MAX];
static int                g_lost[GLIDE_MT_MAX]; /* worker i died or was abandoned */
static int                g_mt_broken;          /* after a failure: single-core */

#define MT_STUCK_US 2000000u /* no progress for this long = hung */

#ifdef GLIDULATOR_DEBUG
/* Test of the safety net (debug build only): GLIDULATOR_MT_TEST=fault or
 * hang makes worker 1 crash or hang at triangle 20000; bsp/gp crash the
 * main core (crash handler of the debug OVL, diag_dos.c). */
static int               g_test_mode; /* 1 fault, 2 hang, 3 bsp, 4 gp (protection fault, Watcom) */
#    if defined(__WATCOMC__)
static void test_gp(void);
#        pragma aux test_gp = "mov ax, 7" "mov es, ax" modify [eax]; /* selector 7: #GP */
#    else
#        define test_gp() ((void) 0)
#    endif
static volatile int      g_test_zero;
#endif

/* PAUSE: relieves the Hyper-Threading sibling while spinning */
#if defined(__GNUC__)
static inline void cpu_relax(void) {
    __asm__ __volatile__("pause" ::: "memory");
}
#    define COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")
#elif defined(__WATCOMC__)
static void cpu_relax(void);
#    pragma aux cpu_relax = 0xf3 0x90;
/* Watcom does not reorder volatile accesses and function calls */
#    define COMPILER_BARRIER() ((void) 0)
#else
#    define cpu_relax()        ((void) 0)
#    define COMPILER_BARRIER() ((void) 0)
#endif

static void worker_main(void *arg) {
    int      id = *(int *) arg;
    uint32_t r  = g_q_done[id];
    for (;;) {
        while (r == g_q_write) {
            if (g_quit)
                return;
            cpu_relax();
        }
        COMPILER_BARRIER();
#ifdef GLIDULATOR_DEBUG
        if (g_test_mode && g_test_mode < 3 && id == 1 && r == 20000) {
            if (g_test_mode == 1)
                g_test_zero = 1 / g_test_zero; /* #DE */
            else
                for (;;)
                    cpu_relax();
        }
#endif
        glide_fsaa_draw(&g_q[r & (MT_QUEUE - 1)].params, id, id);
        r++;
        COMPILER_BARRIER();
        g_q_done[id] = r;
    }
}

/* Index of the slowest worker. Returns the index, not its progress value:
 * looking that value up again in g_q_done[] races with the worker
 * advancing, and the search could run past the array. */
static int slowest(void) {
    int s = 0;
    for (int i = 1; i < g_workers; i++)
        if (g_q_write - g_q_done[i] > g_q_write - g_q_done[s])
            s = i;
    return s;
}

int glide_mt_workers(void) {
    return g_workers;
}

/* Waits until worker i reaches target (distance < slack). 0 = ok, -1 = it
 * died or hangs (g_lost[i] set). Checks the time only every 4096 rounds to
 * keep timer reads off the memory bus. */
static int wait_worker(int i, uint32_t target, uint32_t slack) {
    uint32_t last = g_q_done[i];
    uint64_t t0   = 0;
    unsigned spin = 0;
    while (target - g_q_done[i] > slack) {
        if (g_lost[i])
            return -1;
        cpu_relax();
        if (++spin & 4095)
            continue;
        if (platform_worker_failed(g_w[i])) {
            g_lost[i] = 1;
            return -1;
        }
        uint64_t now = platform_time_us();
        if (g_q_done[i] != last || !t0) {
            last = g_q_done[i];
            t0   = now;
        } else if (now - t0 > MT_STUCK_US && platform_kill_worker(g_w[i])) {
            g_lost[i] = 1;
            return -1;
        }
    }
    return 0;
}

static void mt_recover(void);

#ifdef GLIDULATOR_TRACE
#    define MT_LOG(s) platform_log(s)
#else
#    define MT_LOG(s) ((void) 0)
#endif

void glide_mt_idle(void) {
    if (!g_workers)
        return;
    for (int i = 0; i < g_workers; i++)
        if (wait_worker(i, g_q_write, 0) < 0) {
            mt_recover();
            return;
        }
    COMPILER_BARRIER();
}

/* Failure handling, see the header comment. Runs on the main core while
 * the other workers may still be working through the queue. */
static void mt_recover(void) {
    int n = g_workers;
    MT_LOG("Multi-core: worker stuck, starting recovery\n");
    for (int i = 0; i < n; i++)
        if (!g_lost[i])
            wait_worker(i, g_q_write, 0); /* sets g_lost[i] if needed */
    COMPILER_BARRIER();
    /* Redraw the lost workers' scanlines (render_threads is still n, so
     * exactly their lines), except the triangle they stopped on: it may
     * have caused the failure. */
    for (int i = 0; i < n; i++)
        if (g_lost[i])
            for (uint32_t r = g_q_done[i] + 1; r != g_q_write && r - g_q_done[i] <= MT_QUEUE; r++)
                glide_fsaa_draw(&g_q[r & (MT_QUEUE - 1)].params, i, GLIDE_MT_MAX);
    MT_LOG("Multi-core: lines redrawn\n");
    g_quit = 1;
    for (int i = 0; i < n; i++)
        platform_join_worker(g_w[i]); /* returns immediately for lost workers */
    MT_LOG("Multi-core: worker ended\n");
    /* texture cache: every queued triangle counts as drawn */
    for (int t = 0; t < 2; t++)
        for (int c = 0; c < TEX_CACHE_MAX; c++)
            for (int k = 0; k < GLIDE_MT_MAX; k++)
                g_voodoo.texture_cache[t][c].refcount_r[k] = g_voodoo.texture_cache[t][c].refcount;
#ifdef GLIDULATOR_TRACE
    {
        char  line[96];
        char *p = line;
        for (const char *m = "Multi-core: worker lost, continuing on one core (worker"; *m;)
            *p++ = *m++;
        for (int i = 0; i < n; i++)
            if (g_lost[i]) {
                *p++ = ' ';
                *p++ = (char) ('0' + i);
            }
        *p++ = ')';
        *p++ = '\n';
        *p   = 0;
        platform_log(line);
    }
#endif
    g_workers               = 0;
    g_mt_broken             = 1;
    g_voodoo.render_threads = 1;
    platform_free(g_q);
    g_q = NULL;
}

void glide_mt_submit(const voodoo_params_t *params) {
    for (;;) {
        int i = slowest();
        if (g_q_write - g_q_done[i] < MT_QUEUE)
            break;
        /* queue full: wait for the slowest worker (with safety net) */
        if (wait_worker(i, g_q_write, MT_QUEUE - 1) < 0) {
            static voodoo_params_t copy; /* voodoo_triangle() takes non-const params */
            mt_recover();
            copy = *params;
            glide_fsaa_draw(&copy, 0, GLIDE_MT_MAX);
            return;
        }
    }
    COMPILER_BARRIER();
#ifdef GLIDULATOR_DEBUG
    if (g_test_mode >= 3 && g_q_write == 3000) {
        platform_log("Multi-core test: fault on the main core\n");
        if (g_test_mode == 4)
            test_gp();
        g_test_zero = 1 / g_test_zero; /* #DE on the main core */
    }
#endif
    g_q[g_q_write & (MT_QUEUE - 1)].params = *params;
    COMPILER_BARRIER();
    g_q_write = g_q_write + 1;
}

void glide_mt_stop(void) {
    if (!g_workers)
        return;
    glide_mt_idle();
    g_quit = 1;
    for (int i = 0; i < g_workers; i++)
        platform_join_worker(g_w[i]);
    g_workers = 0;
    platform_free(g_q);
    g_q = NULL;
}

int glide_mt_start(int n) {
    glide_mt_stop();
    if (n < 2 || n > GLIDE_MT_MAX || g_mt_broken)
        return 0;
#ifdef GLIDULATOR_DEBUG
    {
        char cfg[8];
        g_test_mode = 0;
        if (platform_get_config("GLIDULATOR_MT_TEST", cfg, sizeof cfg))
            g_test_mode = cfg[0] == 'f' ? 1 : cfg[0] == 'h' ? 2 : cfg[0] == 'b' ? 3 : cfg[0] == 'g' ? 4 : 0;
    }
#endif
    g_q = (mt_entry *) platform_alloc(sizeof(mt_entry) * MT_QUEUE);
    if (!g_q)
        return 0;
    g_q_write = 0;
    g_quit    = 0;
    for (int i = 0; i < n; i++) {
        g_q_done[i] = 0;
        g_ids[i]    = i;
        g_lost[i]   = 0;
    }
    for (int i = 0; i < n; i++) {
        g_w[i] = platform_spawn_worker(i + 1, worker_main, &g_ids[i]);
        if (!g_w[i]) {
            /* stop the ones already started */
            g_quit = 1;
            for (int k = 0; k < i; k++)
                platform_join_worker(g_w[k]);
            platform_free(g_q);
            g_q = NULL;
            return 0;
        }
    }
    g_workers = n;
    return 1;
}
