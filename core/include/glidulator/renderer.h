#ifndef GLIDULATOR_RENDERER_H
#define GLIDULATOR_RENDERER_H

/* Choice of the render path (docs/technical-reference.md, section 4). Two
 * independent axes:
 *   - Pixel pipeline: the C interpreter from 86Box (always available) or
 *     the JIT, which generates SSE2 code per render-state combination.
 *   - Threads: 1 = render synchronously on the calling core; n >= 2 = n
 *     workers on OTHER cores split every triangle by scanline (line y
 *     belongs to worker y % n, core/render_mt.c) while the game continues.
 * Settings via platform_get_config():
 *   GLIDULATOR_RENDERER = auto | c | jit    (default auto)
 *   GLIDULATOR_THREADS  = auto | 1 | 2 ...  (default auto)
 * Anything unavailable silently falls back to the next best choice
 * (ultimately C on one thread); trace variants log the result. */

typedef enum {
    GLIDE_RENDERER_C = 0,
    GLIDE_RENDERER_JIT,
    GLIDE_RENDERER_COUNT
} glide_renderer_kind;

#define GLIDE_RENDERER_AUTO    (-1)
#define GLIDE_RENDERER_UNKNOWN (-2)

/* Maximum number of render workers (the JIT has code slots per worker index) */
#define GLIDE_MT_MAX 4

/* "c", "jit" */
const char *glide_renderer_name(int kind);
/* Name -> kind; "auto" -> GLIDE_RENDERER_AUTO, otherwise GLIDE_RENDERER_UNKNOWN. */
int glide_renderer_parse(const char *s);
/* Is the path built in and usable on this CPU/platform? */
int glide_renderer_available(int kind);
/* Highest useful thread count: cores other than the calling one, at most
 * GLIDE_MT_MAX; 1 if fewer than two other cores exist. */
int glide_renderer_max_threads(void);

/* For test tools (glide_replay, tests): force a choice instead of reading
 * the configuration. kind = GLIDE_RENDERER_AUTO and threads = 0 mean "from
 * the configuration again". Takes effect at the next glide_renderer_apply(). */
void glide_renderer_force(int kind, int threads);

/* Read the settings, resolve them and configure the rasterizer (and start
 * workers). Called by grSstWinOpen() after g_voodoo has been set up. */
void glide_renderer_apply(void);

/* Result of the last glide_renderer_apply(). */
int glide_renderer_active(void);
int glide_renderer_threads(void);

#endif /* GLIDULATOR_RENDERER_H */
