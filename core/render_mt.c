/* Multi-core rendering: triangles go into a queue; n workers on other
 * cores draw them in the same order, each its own scanlines
 * (g_voodoo.line_owner, vid_voodoo_render.c; like 86Box's render threads,
 * which use y % n). The game
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
 * forever.
 *
 * Load balancing (glide_mt_frame, after every frame): each worker counts
 * the TSC cycles it spends drawing. The frame waits for the slowest
 * worker, so the shares of the scanlines move towards equal drawing
 * times: a worker on a Hyper-Threading sibling of a busy core gets fewer
 * lines. The lines are handed out in bands of MT_BAND lines (equal shares:
 * band k to drawer k % n); a drawer whose lines a triangle does not touch
 * skips it before any setup (voodoo_triangle(), line_count). The picture
 * does not depend on the distribution (every worker draws every triangle
 * on its lines in the same order).
 *
 * The main core draws too (with at most GLIDE_MT_MAX - 1 workers: the JIT
 * and the texture cache have GLIDE_MT_MAX slots): it gets the drawer index
 * g_workers and its own scanlines, and draws them only when it would
 * otherwise wait -- at synchronisation points, when the queue is full for
 * it, and while it waits for a worker. Its share follows from the game's
 * own time G per frame (main core busy, neither drawing nor waiting):
 * the main core needs G + its drawing, a worker only its drawing. When G
 * alone exceeds the workers' drawing time it gets no lines and does not
 * take part (render_threads = g_workers). */

#include "glidulator/platform.h"
#include "glidulator/renderer.h"

#include "glide_internal.h"
#include "vid_voodoo_render.h"

#define MT_QUEUE 256 /* power of two */

#if GLIDE_MT_MAX != VOODOO_MAX_DRAWERS
#    error GLIDE_MT_MAX and VOODOO_MAX_DRAWERS must match
#endif

/* A triangle, or a job (job != NULL): every drawer calls job(drawer, arg)
 * once, which works on the lines glide_mt_line_owner() gives that drawer
 * (glide_mt_parallel, e.g. the video filter). */
typedef struct {
    voodoo_params_t params;
    glide_mt_job_fn job;
    void           *arg;
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

/* Drawing time per worker since the last glide_mt_frame (TSC cycles), one
 * cache line each: only worker i writes entry i. */
static volatile struct {
    uint64_t cycles;
    char     pad[56];
} g_busy[GLIDE_MT_MAX];

/* Share of worker i in 1/MT_SHARE_ONE of the scanlines; damped, the
 * table g_voodoo.line_owner is rebuilt only when the line counts change. */
#define MT_SHARE_ONE   65536u
#define MT_BAND        4 /* lines per band of the owner table */
#define MT_MIN_CYCLES  2000000u /* frames with less drawing are not measured */
static uint32_t g_share[GLIDE_MT_MAX];
static int      g_lines[GLIDE_MT_MAX];
static int      g_balance = 1; /* 0: fixed equal shares (trace builds: GLIDULATOR_MT_BALANCE=0) */
static int      g_band    = MT_BAND; /* lines per band of the owner table (trace builds: GLIDULATOR_MT_BAND) */

/* Main core as drawer (index g_workers, see the header comment) */
static int      g_main_allowed = 1; /* trace builds: GLIDULATOR_MT_MAIN=0 */
static int      g_main_on;          /* main core has lines */
static uint32_t g_main_done;        /* triangles the main core has drawn */
static uint64_t g_main_cycles;      /* its drawing time this frame */
static uint64_t g_wait_cycles;      /* its waiting time this frame */
static uint64_t g_render_start;     /* first triangle of the frame, 0 = none yet */
/* switch on from this target, off below this share (5 % and 1.7 % of
 * the lines) */
#define MT_MAIN_ON_LINES  (VOODOO_LINE_PERIOD / 20)
#define MT_MAIN_OFF_LINES (VOODOO_LINE_PERIOD / 60)

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

/* Cycle counter for the workers: platform_time_us() may not be callable on
 * their cores (Windows 9x: GLSMP.VXD runs them in ring 0 next to the
 * system). Workers only exist on CPUs with TSC (smp_dos.c checks it;
 * Windows/Linux: any multi-core CPU). Only differences on one core count. */
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
static inline uint64_t cycles(void) {
    return __builtin_ia32_rdtsc();
}
#elif defined(__WATCOMC__)
static void rdtsc_mt(uint32_t *lo_hi);
#    pragma aux rdtsc_mt = 0x0f 0x31 "mov [ecx], eax" "mov [ecx+4], edx" parm [ecx] modify [eax edx];
static uint64_t cycles(void) {
    uint32_t v[2];
    rdtsc_mt(v);
    return ((uint64_t) v[1] << 32) | v[0];
}
#else
static uint64_t cycles(void) {
    return platform_time_us() * 1000u;
}
#endif

/* Spreads g_lines[] over the period like a smooth weighted round robin
 * in bands of g_band lines: each drawer's bands lie as evenly apart as
 * possible; equal counts give 0, 1, ..., n-1, 0, 1, ... per band (g_band =
 * 1: y % n). A drawer then owns about g_lines[i] lines (exactly for g_band
 * 1). Bands let a drawer skip small triangles without a line of its own
 * (line_count, voodoo_triangle()). */
static void build_table(int n) {
    int cur[GLIDE_MT_MAX] = { 0 };
    for (int y = 0; y < VOODOO_LINE_PERIOD; y += g_band) {
        int best = 0;
        for (int i = 0; i < n; i++) {
            cur[i] += g_lines[i];
            if (cur[i] > cur[best])
                best = i;
        }
        cur[best] -= VOODOO_LINE_PERIOD;
        for (int k = y; k < y + g_band && k < VOODOO_LINE_PERIOD; k++)
            g_voodoo.line_owner[k] = (uint8_t) best;
    }
    for (int i = 0; i < GLIDE_MT_MAX; i++) {
        uint16_t *cnt = g_voodoo.line_count[i];
        cnt[0]        = 0;
        for (int y = 0; y < VOODOO_LINE_PERIOD; y++)
            cnt[y + 1] = (uint16_t) (cnt[y] + (g_voodoo.line_owner[y] == i));
    }
}

/* Line counts from g_share (largest remainder, sum = period); 1 if they
 * changed. */
static int shares_to_lines(int n) {
    int      lines[GLIDE_MT_MAX], sum = 0, changed = 0;
    uint32_t rem[GLIDE_MT_MAX];
    for (int i = 0; i < n; i++) {
        uint32_t v = g_share[i] * (uint32_t) VOODOO_LINE_PERIOD; /* <= 2^16 * 840 < 2^32 */
        lines[i]   = (int) (v / MT_SHARE_ONE);
        rem[i]     = v % MT_SHARE_ONE;
        sum += lines[i];
    }
    while (sum < VOODOO_LINE_PERIOD) {
        int best = 0;
        for (int i = 1; i < n; i++)
            if (rem[i] > rem[best])
                best = i;
        lines[best]++;
        rem[best] = 0;
        sum++;
    }
    for (int i = 0; i < n; i++) {
        if (lines[i] != g_lines[i])
            changed = 1;
        g_lines[i] = lines[i];
    }
    return changed;
}

static void set_main(int on);

/* Drawers: the workers plus the main core while it has lines */
static int drawers(void) {
    return g_workers + g_main_on;
}

static void balance_reset(int n) {
    uint32_t left = MT_SHARE_ONE;
    for (int i = 0; i < n; i++) {
        g_share[i] = left / (uint32_t) (n - i);
        left -= g_share[i];
        g_lines[i]       = VOODOO_LINE_PERIOD / n; /* period is a multiple of 2, 3, 4 */
        g_busy[i].cycles = 0;
    }
    g_main_on      = 0;
    g_main_cycles  = 0;
    g_wait_cycles  = 0;
    g_render_start = 0;
    build_table(n);
}

/* The main core draws its scanlines of the next queued triangle */
static void main_draw_one(void) {
    int       id = g_workers;
    mt_entry *e  = &g_q[g_main_done & (MT_QUEUE - 1)];
    if (e->job)
        e->job(id, e->arg);
    else {
        uint64_t t0 = cycles();
        glide_fsaa_draw(&e->params, id, id);
        g_main_cycles += cycles() - t0;
    }
    g_main_done++;
}

static void main_catch_up(void) {
    if (g_main_on)
        while (g_main_done != g_q_write)
            main_draw_one();
}

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
        mt_entry *e = &g_q[r & (MT_QUEUE - 1)];
        if (e->job)
            e->job(id, e->arg); /* not part of the drawing time (balance) */
        else {
            uint64_t t0 = cycles();
            glide_fsaa_draw(&e->params, id, id);
            g_busy[id].cycles += cycles() - t0;
        }
        r++;
        COMPILER_BARRIER();
        g_q_done[id] = r;
    }
}

int glide_mt_workers(void) {
    return g_workers;
}

int glide_mt_lines(int i) {
    return i >= 0 && i < drawers() ? g_lines[i] : 0;
}

void glide_mt_test_lines(const int *lines) {
    int n = g_workers;
    set_main(n < GLIDE_MT_MAX && lines[n] > 0);
    for (int i = 0; i < drawers(); i++)
        g_lines[i] = lines[i];
    build_table(drawers());
    g_balance = 0;
}

/* Waits until worker i reaches target (distance < slack). 0 = ok, -1 = it
 * died or hangs (g_lost[i] set). Checks the time only every 4096 rounds to
 * keep timer reads off the memory bus. */
static int wait_worker_spin(int i, uint32_t target, uint32_t slack);

/* Waiting time is measured for the balance (main core idle); the main
 * core draws its own lines meanwhile. */
static int wait_worker(int i, uint32_t target, uint32_t slack) {
    if (target - g_q_done[i] <= slack)
        return 0;
    uint64_t t0 = cycles(), m0 = g_main_cycles;
    int      r  = wait_worker_spin(i, target, slack);
    g_wait_cycles += (cycles() - t0) - (g_main_cycles - m0);
    return r;
}

static int wait_worker_spin(int i, uint32_t target, uint32_t slack) {
    uint32_t last = g_q_done[i];
    uint64_t t0   = 0;
    unsigned spin = 0;
    while (target - g_q_done[i] > slack) {
        if (g_lost[i])
            return -1;
        if (g_main_on && g_main_done != g_q_write) {
            main_draw_one(); /* own lines instead of spinning */
            continue;
        }
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
    main_catch_up();
    for (int i = 0; i < g_workers; i++)
        if (wait_worker(i, g_q_write, 0) < 0) {
            mt_recover();
            return;
        }
    COMPILER_BARRIER();
}

#ifdef GLIDULATOR_TRACE
static char *put_num(char *p, int v) {
    if (v >= 1000)
        *p++ = (char) ('0' + v / 1000 % 10);
    if (v >= 100)
        *p++ = (char) ('0' + v / 100 % 10);
    if (v >= 10)
        *p++ = (char) ('0' + v / 10 % 10);
    *p++ = (char) ('0' + v % 10);
    return p;
}
#endif

/* Switches the main core's lines on or off; only while nothing is in
 * flight (glide_mt_frame after glide_mt_idle). */
static void set_main(int on) {
    if (on == g_main_on)
        return;
    if (on) {
        /* its texture-cache counter starts in step with the queue */
        for (int t = 0; t < 2; t++)
            for (int c = 0; c < TEX_CACHE_MAX; c++)
                g_voodoo.texture_cache[t][c].refcount_r[g_workers] = g_voodoo.texture_cache[t][c].refcount;
        g_main_done = g_q_write;
    }
    g_main_on               = on;
    g_voodoo.render_threads = g_workers + on;
}

/* After glide_mt_idle() at the end of a frame (no worker draws). Model:
 * drawer i needs G_i + c_i * L_i for its L_i lines (c_i = cycles per line
 * measured this frame, G = game time of the main core, 0 for workers).
 * The frame ends when the last one is done, so the target gives every
 * drawer the same finish time T. The shares move halfway towards it; a
 * worker keeps at least a quarter of an even share so it stays
 * measurable. With equal workers and no main core this is the fixed point
 * "equal drawing times". */
void glide_mt_frame(void) {
    int n = g_workers;
    if (!n || !g_balance)
        return;
    uint64_t busy[GLIDE_MT_MAX], total = 0;
    uint64_t start = g_render_start, now = cycles();
    uint64_t main_c = g_main_cycles, wait_c = g_wait_cycles;
    for (int i = 0; i < n; i++) {
        busy[i]          = g_busy[i].cycles;
        g_busy[i].cycles = 0;
        total += busy[i];
    }
    g_main_cycles = g_wait_cycles = 0;
    g_render_start                = 0;
    int main_ok = g_main_allowed && n < GLIDE_MT_MAX && start;
    if (n < 2 && !main_ok)
        return;
    for (int i = 0; i < n; i++)
        if (!busy[i] || !g_lines[i])
            return; /* no work for this worker: nothing to learn */
    if (total < MT_MIN_CYCLES * (uint64_t) n)
        return;
    double c[GLIDE_MT_MAX], inv = 0.0, cw = 0.0;
    for (int i = 0; i < n; i++) {
        c[i] = (double) busy[i] / (double) g_lines[i];
        inv += 1.0 / c[i];
        cw += c[i];
    }
    cw /= n;
    double L = (double) VOODOO_LINE_PERIOD;
    double target[GLIDE_MT_MAX];
    double t_main = 0.0;
    if (main_ok) {
        double span = (double) (now - start);
        double g    = span - (double) wait_c - (double) main_c;
        double cm   = g_main_on && g_lines[n] && main_c ? (double) main_c / (double) g_lines[n] : cw;
        if (g < 0.0)
            g = 0.0;
        double T = (L + g / cm) / (inv + 1.0 / cm);
        if (T > g)
            t_main = (T - g) / cm;
    }
    double rest = L - t_main;
    for (int i = 0; i < n; i++)
        target[i] = rest * (1.0 / c[i]) / inv;
    /* main core: switch with hysteresis, new share damped */
    double old_main = g_main_on ? (double) g_share[n] * L / MT_SHARE_ONE : 0.0;
    double new_main = (old_main + t_main) / 2.0;
    int    on       = g_main_on ? new_main >= MT_MAIN_OFF_LINES : t_main >= MT_MAIN_ON_LINES;
    if (!on)
        new_main = 0.0;
    uint32_t floor_ = MT_SHARE_ONE / (uint32_t) (4 * n), sum = 0;
    uint32_t main_share = (uint32_t) (new_main / L * MT_SHARE_ONE);
    for (int i = 0; i < n; i++) {
        uint32_t t = (uint32_t) (target[i] / L * MT_SHARE_ONE);
        uint32_t v = (g_share[i] + t) / 2;
        g_share[i] = v < floor_ ? floor_ : v;
        sum += g_share[i];
    }
    if (on) {
        g_share[n] = main_share;
        sum += main_share;
    }
    /* renormalise (rounding, floor): rest to/from the largest worker */
    int big = 0;
    for (int i = 1; i < n; i++)
        if (g_share[i] > g_share[big])
            big = i;
    g_share[big] += MT_SHARE_ONE - sum; /* unsigned wrap handles sum > ONE */
    set_main(on);
    if (shares_to_lines(drawers()))
        build_table(drawers());
#ifdef GLIDULATOR_TRACE
    {
        static unsigned frames;
        if (!(++frames & 255)) {
            char  line[128];
            char *p = line;
            for (const char *m = "Multi-core: lines per 840 by worker:"; *m;)
                *p++ = *m++;
            for (int i = 0; i < n; i++) {
                *p++ = ' ';
                p    = put_num(p, g_lines[i]);
            }
            for (const char *m = ", main core "; *m;)
                *p++ = *m++;
            p    = put_num(p, g_main_on ? g_lines[n] : 0);
            *p++ = '\n';
            *p   = 0;
            platform_log(line);
        }
    }
#endif
}

/* Failure handling, see the header comment. Runs on the main core while
 * the other workers may still be working through the queue. */
static void mt_recover(void) {
    int n = g_workers;
    MT_LOG("Multi-core: worker stuck, starting recovery\n");
    main_catch_up(); /* the main core's own lines */
    g_main_on = 0;
    for (int i = 0; i < n; i++)
        if (!g_lost[i])
            wait_worker(i, g_q_write, 0); /* sets g_lost[i] if needed */
    COMPILER_BARRIER();
    /* Redraw the lost workers' scanlines (render_threads is still n, so
     * exactly their lines), except the triangle they stopped on: it may
     * have caused the failure. */
    for (int i = 0; i < n; i++)
        if (g_lost[i])
            for (uint32_t r = g_q_done[i] + 1; r != g_q_write && r - g_q_done[i] <= MT_QUEUE; r++) {
                mt_entry *e = &g_q[r & (MT_QUEUE - 1)];
                if (e->job)
                    e->job(i, e->arg); /* lost worker's lines */
                else
                    glide_fsaa_draw(&e->params, i, GLIDE_MT_MAX);
            }
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

/* Waits until the next queue entry may be written. 0 = ok, -1 = a worker
 * failed and mt_recover() ran (single-core from now on). */
static int queue_reserve(void) {
    /* The entry about to be written was triangle g_q_write - MT_QUEUE:
     * EVERY worker must be past it. Checking only the slowest worker found
     * by a search is not enough: with a tie the search picks the lower
     * index, that worker advances, and the other one is still drawing the
     * entry that gets overwritten (texture-cache counters then disagree
     * and voodoo_use_texture() waits forever; Half-Life, Windows 98).
     * Lags only shrink while we wait, so one pass suffices. The main core
     * draws its own lines of that entry first. */
    if (g_main_on && g_q_write - g_main_done >= MT_QUEUE)
        main_draw_one();
    for (int i = 0; i < g_workers; i++) {
        if (g_q_write - g_q_done[i] < MT_QUEUE)
            continue;
        /* queue full: wait for this worker (with safety net) */
        if (wait_worker(i, g_q_write, MT_QUEUE - 1) < 0) {
            mt_recover();
            return -1;
        }
    }
    COMPILER_BARRIER();
    return 0;
}

void glide_mt_submit(const voodoo_params_t *params) {
    if (!g_render_start)
        g_render_start = cycles();
    if (queue_reserve() < 0) {
        static voodoo_params_t copy; /* voodoo_triangle() takes non-const params */
        copy = *params;
        glide_fsaa_draw(&copy, 0, GLIDE_MT_MAX);
        return;
    }
#ifdef GLIDULATOR_DEBUG
    if (g_test_mode >= 3 && g_q_write == 3000) {
        platform_log("Multi-core test: fault on the main core\n");
        if (g_test_mode == 4)
            test_gp();
        g_test_zero = 1 / g_test_zero; /* #DE on the main core */
    }
#endif
    mt_entry *e = &g_q[g_q_write & (MT_QUEUE - 1)];
    e->params   = *params;
    e->job      = 0;
    COMPILER_BARRIER();
    g_q_write = g_q_write + 1;
}

int glide_mt_line_owner(int y) {
    if (g_voodoo.render_threads < 2)
        return 0;
    return g_voodoo.line_owner[(uint32_t) (y & 0x7fffffff) % VOODOO_LINE_PERIOD];
}

/* Runs fn(drawer, arg) on every drawer and returns when all are done.
 * Not counted in the balance: the drawing and waiting times of the frame
 * stay as they were (the job runs between frames, in grBufferSwap). */
void glide_mt_parallel(glide_mt_job_fn fn, void *arg) {
    if (!g_workers) {
        fn(0, arg);
        return;
    }
    uint64_t wait_c = g_wait_cycles;
    if (queue_reserve() < 0) {
        fn(0, arg); /* now single-core: drawer 0 owns every line */
        return;
    }
    mt_entry *e = &g_q[g_q_write & (MT_QUEUE - 1)];
    e->job      = fn;
    e->arg      = arg;
    COMPILER_BARRIER();
    g_q_write = g_q_write + 1;
    glide_mt_idle(); /* the main core does its share in there */
    g_wait_cycles = wait_c;
}

void glide_mt_stop(void) {
    if (!g_workers)
        return;
    glide_mt_idle();
    g_quit = 1;
    for (int i = 0; i < g_workers; i++)
        platform_join_worker(g_w[i]);
    set_main(0); /* render_threads back to the workers, as before */
    g_workers = 0;
    platform_free(g_q);
    g_q = NULL;
}

int glide_mt_start(int n) {
    glide_mt_stop();
    if (n < 1 || n > GLIDE_MT_MAX || g_mt_broken)
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
    g_balance      = 1;
    g_main_allowed = 1;
    g_band         = MT_BAND;
#ifdef GLIDULATOR_TRACE
    {
        char cfg[4];
        if (platform_get_config("GLIDULATOR_MT_BALANCE", cfg, sizeof cfg) && cfg[0] == '0')
            g_balance = 0;
        if (platform_get_config("GLIDULATOR_MT_BAND", cfg, sizeof cfg) && cfg[0] >= '1' && cfg[0] <= '9') {
            g_band = cfg[0] - '0';
            if (cfg[1] >= '0' && cfg[1] <= '9')
                g_band = g_band * 10 + cfg[1] - '0';
        }
        if (platform_get_config("GLIDULATOR_MT_MAIN", cfg, sizeof cfg) && cfg[0] == '0')
            g_main_allowed = 0;
    }
#endif
    balance_reset(n); /* the workers read the table: before they start */
    /* Texture cache: nothing is in flight (glide_mt_stop waited), so every
     * reference counts as drawn for every worker. Otherwise a change of
     * the worker count (glide_renderer_apply) leaves stale counters for
     * the new workers, and the first full cache waits forever (seen in
     * glide_replay --compare). */
    for (int t = 0; t < 2; t++)
        for (int c = 0; c < TEX_CACHE_MAX; c++)
            for (int k = 0; k < GLIDE_MT_MAX; k++)
                g_voodoo.texture_cache[t][c].refcount_r[k] = g_voodoo.texture_cache[t][c].refcount;
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
