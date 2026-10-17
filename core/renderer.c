/* Choice of the render path, see glidulator/renderer.h. No C runtime (the
 * OVL has no initialised CRT). */

#include "glidulator/renderer.h"
#include "glidulator/platform.h"
#include "glidulator/version.h"

#include "glide_internal.h"
#include "vid_voodoo_render.h"

/* JIT (core/rasterizer/vid_voodoo_codegen_x86.h) on 32-bit x86 only */
#ifndef NO_CODEGEN
#    define GLIDULATOR_HAVE_JIT 1
#else
#    define GLIDULATOR_HAVE_JIT 0
#endif

static int g_force_kind    = GLIDE_RENDERER_AUTO;
static int g_force_threads = 0;
static int g_active        = GLIDE_RENDERER_C;
static int g_threads       = 1;

static const char *const g_names[GLIDE_RENDERER_COUNT] = { "c", "jit" };

static int lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int same(const char *a, const char *b) {
    while (*a && lower((unsigned char) *a) == lower((unsigned char) *b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

const char *glide_renderer_name(int kind) {
    if (kind == GLIDE_RENDERER_AUTO)
        return "auto";
    return (kind >= 0 && kind < GLIDE_RENDERER_COUNT) ? g_names[kind] : "?";
}

int glide_renderer_parse(const char *s) {
    if (same(s, "auto"))
        return GLIDE_RENDERER_AUTO;
    for (int k = 0; k < GLIDE_RENDERER_COUNT; k++)
        if (same(s, g_names[k]))
            return k;
    return GLIDE_RENDERER_UNKNOWN;
}

int glide_renderer_available(int kind) {
    switch (kind) {
    case GLIDE_RENDERER_C:
        return 1;
    case GLIDE_RENDERER_JIT:
        return GLIDULATOR_HAVE_JIT && (platform_cpu_features() & PLATFORM_CPU_SSE2);
    default:
        return 0;
    }
}

int glide_renderer_max_threads(void) {
    int n = platform_core_count() - 1;
    if (n > GLIDE_MT_MAX)
        n = GLIDE_MT_MAX;
    return n >= 2 ? n : 1;
}

void glide_renderer_force(int kind, int threads) {
    g_force_kind    = kind;
    g_force_threads = threads;
}

/* "auto" -> 0, number -> number, garbage -> 0 */
static int parse_threads(const char *s) {
    int n = 0;
    for (; *s >= '0' && *s <= '9'; s++)
        n = n * 10 + (*s - '0');
    return (*s || n > 64) ? 0 : n;
}

#ifdef GLIDULATOR_TRACE
static char *put(char *p, char *end, const char *s) {
    while (*s && p < end)
        *p++ = *s++;
    return p;
}

static char *put_int(char *p, char *end, int v) {
    char tmp[12];
    int  n = 0;
    do
        tmp[n++] = (char) ('0' + v % 10);
    while ((v /= 10) && n < 11);
    while (n && p < end)
        *p++ = tmp[--n];
    return p;
}

static void log_choice(int want, int want_threads) {
    char     line[160];
    char    *end = line + sizeof line - 2;
#ifdef GLIDULATOR_DEBUG
    char    *p   = put(line, end, "Glidulator GL-1 " GLIDULATOR_VERSION " (debug)\nRenderer: ");
#else
    char    *p   = put(line, end, "Glidulator GL-1 " GLIDULATOR_VERSION " (trace)\nRenderer: ");
#endif
    unsigned cpu = platform_cpu_features();
    p            = put(p, end, glide_renderer_name(g_active));
    if (want != GLIDE_RENDERER_AUTO && want != g_active) {
        p = put(p, end, " (requested ");
        p = put(p, end, want == GLIDE_RENDERER_UNKNOWN ? "unknown" : glide_renderer_name(want));
        p = put(p, end, ", not available)");
    }
    p = put(p, end, ", Threads ");
    p = put_int(p, end, g_threads);
    if (want_threads && want_threads != g_threads) {
        p = put(p, end, " (requested ");
        p = put_int(p, end, want_threads);
        p = put(p, end, ")");
    }
    p = put(p, end, ", CPU:");
    if (cpu & PLATFORM_CPU_MMX)
        p = put(p, end, " MMX");
    if (cpu & PLATFORM_CPU_SSE)
        p = put(p, end, " SSE");
    if (cpu & PLATFORM_CPU_SSE2)
        p = put(p, end, " SSE2");
    if (!cpu)
        p = put(p, end, " -");
    p    = put(p, end, "\n");
    *p   = 0;
    platform_log(line);
}
#endif

void glide_renderer_apply(void) {
    char cfg[16];
    int  want         = g_force_kind;
    int  want_threads = g_force_threads;
    /* Stop running workers first: they read render_threads and the JIT
     * cache, which are about to change. */
    glide_mt_stop();
    if (want == GLIDE_RENDERER_AUTO && platform_get_config("GLIDULATOR_RENDERER", cfg, sizeof cfg))
        want = glide_renderer_parse(cfg);
    if (!want_threads && platform_get_config("GLIDULATOR_THREADS", cfg, sizeof cfg))
        want_threads = parse_threads(cfg);

    if (want >= 0 && glide_renderer_available(want))
        g_active = want;
    else
        g_active = glide_renderer_available(GLIDE_RENDERER_JIT) ? GLIDE_RENDERER_JIT : GLIDE_RENDERER_C;

    int max   = glide_renderer_max_threads();
    g_threads = (want_threads <= 0 || want_threads > max) ? max : want_threads;

#ifndef NO_CODEGEN
    /* Hook up the code blocks (g_voodoo is zeroed by grSstWinOpen); without
     * executable memory fall back to C. */
    if (g_active == GLIDE_RENDERER_JIT && !voodoo_codegen_init(&g_voodoo))
        g_active = GLIDE_RENDERER_C;
#endif
    g_voodoo.use_recompiler = (g_active == GLIDE_RENDERER_JIT);
    g_voodoo.render_threads = g_threads;
    /* Start workers only now (they read render_threads); if that fails,
     * continue on one core. */
    if (g_threads >= 2 && !glide_mt_start(g_threads)) {
        g_threads               = 1;
        g_voodoo.render_threads = 1;
    }
#ifdef GLIDULATOR_TRACE
    log_choice(want, want_threads);
#endif
}

int glide_renderer_active(void) {
    return g_active;
}

int glide_renderer_threads(void) {
    return glide_mt_workers() ? g_threads : 1; /* 1 after a worker failure */
}

int glide_renderer_cores(void) {
    int t = glide_renderer_threads();
    return t >= 2 ? t + 1 : 1; /* workers plus the game's core */
}
