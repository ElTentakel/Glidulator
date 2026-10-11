#ifndef GLIDULATOR_TRACE_H
#define GLIDULATOR_TRACE_H

/* Call trace of Glide functions: shows what a real game actually calls.
 * Active only when built with -DGLIDULATOR_TRACE (trace and debug
 * variants); otherwise GLIDE_TRACE() expands to nothing.
 *
 * Output via platform_log(): the first call of every function immediately,
 * call counters at glide_trace_dump() (buffer swap 1, 10, 100, 1000, ...
 * and grGlideShutdown). No stdio: the OVL has no initialised C runtime. */

typedef struct glide_trace_site {
    const char              *name;
    unsigned long            count;
    struct glide_trace_site *next;
} glide_trace_site;

void glide_trace_hit(glide_trace_site *site);
void glide_trace_dump(const char *why);
void glide_trace_swap(void);
/* Free-form note with two numbers, e.g. ("grSstWinOpen", 320, 240). */
void glide_trace_note(const char *what, unsigned long a, unsigned long b);
/* Up to 8 floating-point values with 3 decimals (without printf). */
void glide_trace_floats(const char *what, const float *v, int n);

/* > 0 while Glidulator itself draws through the Glide functions (start-up
 * animation): no trace counting, no frame capture. */
extern int g_glide_mute;

#ifdef GLIDULATOR_TRACE
#    define GLIDE_TRACE(fname)                                   \
        do {                                                     \
            static glide_trace_site glide_trace_site_ = { fname, 0, 0 }; \
            glide_trace_hit(&glide_trace_site_);                 \
        } while (0)
#else
#    define GLIDE_TRACE(fname) ((void) 0)
#endif

#endif /* GLIDULATOR_TRACE_H */
